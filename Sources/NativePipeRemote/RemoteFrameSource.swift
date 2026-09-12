import Accelerate
import CoreVideo
import Foundation
import IOSurface
import Metal
import NativePipeProtocol
import NativePipeWindowing

/// Thread-safe remote frame store. AV1/H.264 parsing and decoder submission
/// stay on `decodeQueue`; only the finished IOSurface is observed by AppKit.
public final class RemoteFrameSource: @unchecked Sendable, FrameSource {
    // Both decoders finish one access unit on decodeQueue before returning.
    private final class Stream: @unchecked Sendable {
        let decoder = H264Decoder()
        let av1 = AV1Decoder()
        var codec: MediaWire.Codec?
        func reset() { decoder.reset(); av1.reset() }
        let generation: UInt64
        init(generation: UInt64) { self.generation = generation }
        var epoch: UInt16 = 0
        var resourceIDs: [UInt32] = []
        var publishedThrough: UInt32?
        var pendingIDs: [UInt32] = []
        var expectedAlpha: Set<UInt32> = []
        var alphaPlanes: [UInt32: AlphaPlane] = [:]
        var lastAlpha: AlphaPlane?
        var pendingVideo: [UInt32: CVPixelBuffer] = [:]
    }

    private struct AlphaPlane {
        let width: Int
        let height: Int
        let bytes: Data
    }

    private final class StoredFrame {
        let surface: IOSurfaceRef
        let surfaceID: UInt32
        let pixelBuffer: CVPixelBuffer?
        var texture: MTLTexture?

        init(surface: IOSurfaceRef, surfaceID: UInt32, pixelBuffer: CVPixelBuffer?) {
            self.surface = surface
            self.surfaceID = surfaceID
            self.pixelBuffer = pixelBuffer
        }
    }

    private let lock = NSLock()
    private let decodeQueue = DispatchQueue(
        label: "com.nativepipe.remote.decode", qos: .userInteractive)
    nonisolated(unsafe) private var streams: [UInt32: Stream] = [:]
    nonisolated(unsafe) private var frames: [UInt32: StoredFrame] = [:]
    nonisolated(unsafe) private var destroyedSurfaces: Set<UInt32> = []
    nonisolated(unsafe) private var frameAvailable: (@MainActor (UInt32) -> Void)?
    nonisolated(unsafe) private var queuedMediaBytes = 0
    nonisolated(unsafe) private var hardwareFrames = 0
    nonisolated(unsafe) private var softwareFrames = 0
    public var decoderFrameCounts: (hardware: Int, software: Int) {
        lock.lock(); defer { lock.unlock() }
        return (hardwareFrames, softwareFrames)
    }
    private let device = MTLCreateSystemDefaultDevice()
    private let maximumFramesPerStream = 12
    nonisolated(unsafe) private var generation: UInt64 = 0
    nonisolated(unsafe) private var notificationScheduled = false
    nonisolated(unsafe) private var newestNotification: UInt32?
    nonisolated(unsafe) private var decodingFailed = false
    @MainActor var onFailure: ((String) -> Void)?
    private struct TextureRequest {
        let generation: UInt64
        let layers: [Windowing.SceneLayer]
        var results: [FrameTextureResolution]
        let completion: @MainActor ([FrameTextureResolution]) -> Void
    }
    @MainActor private var textureRequests: [TextureRequest] = []
    @MainActor private var consumptionRequests: [(generation: UInt64, layers: [Windowing.SceneLayer], completion: @MainActor () -> Void)] = []

    /// Discarding metadata does not consume its queued compressed input. Remote
    /// display credit is returned only after decode/alpha has crossed it.
    @MainActor public func whenConsumed(_ layers: [Windowing.SceneLayer], completion: @escaping @MainActor () -> Void) {
        lock.lock(); let token = generation; lock.unlock()
        if consumed(layers) { completion() }
        else { consumptionRequests.append((token, layers, completion)) }
    }

    @MainActor private func consumed(_ layers: [Windowing.SceneLayer]) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return layers.allSatisfy { layer in
            if destroyedSurfaces.contains(layer.surface) { return true }
            guard let latest = streams[layer.surface]?.publishedThrough else { return false }
            return Int32(bitPattern: latest &- layer.resourceID) >= 0
        }
    }

    public init() {}

    nonisolated private func fail(_ message: String, generation token: UInt64) {
        lock.lock()
        guard generation == token, !decodingFailed else { lock.unlock(); return }
        decodingFailed = true
        lock.unlock()
        MainRunLoop.perform { [weak self] in
            guard let self else { return }
            self.lock.lock(); let current = self.generation == token && self.decodingFailed; self.lock.unlock()
            if current { self.onFailure?(message) }
        }
    }

    @MainActor
    public func setFrameAvailableHandler(_ handler: @escaping @MainActor (UInt32) -> Void) {
        lock.lock()
        frameAvailable = handler
        lock.unlock()
    }

    @MainActor
    public func surface(forResource resourceID: UInt32) -> IOSurfaceRef? {
        lock.lock(); defer { lock.unlock() }
        return frames[resourceID]?.surface
    }

    @MainActor
    public func isResourcePublished(_ resourceID: UInt32) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return frames[resourceID] != nil
    }

    @MainActor
    public func metalTexture(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> AnyObject? {
        resolveFrame(forResource: resourceID, width: width, height: height,
                     bytesPerRow: bytesPerRow, format: format).texture
    }

    @MainActor
    public func resolveFrame(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> FrameTextureResolution {
        lock.lock(); defer { lock.unlock() }
        guard let frame = frames[resourceID] else {
            return FrameTextureResolution(status: .unpublished)
        }
        guard width > 0, height > 0,
              IOSurfaceGetWidth(frame.surface) >= width,
              IOSurfaceGetHeight(frame.surface) >= height else {
            return FrameTextureResolution(status: .unavailable)
        }
        if frame.texture?.width != width || frame.texture?.height != height {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
            descriptor.storageMode = .shared
            descriptor.usage = [.shaderRead]
            frame.texture = device?.makeTexture(descriptor: descriptor, iosurface: frame.surface, plane: 0)
        }
        // Each stream advances in decode order. Older resolved frames are now
        // owned by their consumers, not a twelve-frame history cache. Keep this
        // current resource for later scenes that reuse a static sublayer.
        if let stream = streams[frame.surfaceID],
           let index = stream.resourceIDs.firstIndex(of: resourceID), index > 0 {
            for retired in stream.resourceIDs.prefix(index) { frames.removeValue(forKey: retired) }
            stream.resourceIDs.removeFirst(index)
        }
        return FrameTextureResolution(status: .ready, texture: frame.texture,
                                      surface: frame.surface, owner: frame)
    }

    @MainActor
    public func metalTextures(
        for layers: [Windowing.SceneLayer],
        completion: @escaping @MainActor ([FrameTextureResolution]) -> Void
    ) {
        let results = layers.map(resolveLayer)
        guard results.contains(where: { $0.status == .unpublished }) else { completion(results); return }
        lock.lock(); let token = generation; lock.unlock()
        // WindowBridge permits one lookup per owner. Retain complete sublayers
        // while waiting; cache retirement cannot tear a partly resolved scene.
        textureRequests.append(.init(generation: token, layers: layers, results: results, completion: completion))
    }

    @MainActor private func resolveLayer(_ layer: Windowing.SceneLayer) -> FrameTextureResolution {
        let result = resolveFrame(forResource: layer.resourceID, width: layer.width, height: layer.height,
                                  bytesPerRow: layer.bytesPerRow, format: 1)
        guard result.status == .unpublished else { return result }
        lock.lock(); defer { lock.unlock() }
        // An evicted or skipped decoded frame will never arrive again. Complete
        // this lookup so WindowBridge can advance to its newer candidate.
        if destroyedSurfaces.contains(layer.surface) { return .init(status: .unavailable) }
        if let latest = streams[layer.surface]?.publishedThrough,
           Int32(bitPattern: latest &- layer.resourceID) >= 0 { return .init(status: .unavailable) }
        return result
    }

    @MainActor private func deliverAvailableFrames() {
        lock.lock()
        notificationScheduled = false
        let resourceID = newestNotification, token = generation, callback = frameAvailable
        let destroyed = destroyedSurfaces
        newestNotification = nil
        lock.unlock()
        let requests = textureRequests
        textureRequests.removeAll()
        var completed: [TextureRequest] = []
        for var request in requests {
            for index in request.results.indices {
                if request.generation != token || destroyed.contains(request.layers[index].surface) {
                    request.results[index] = .init(status: .unavailable)
                } else if request.results[index].status == .unpublished {
                    let layer = request.layers[index]
                    request.results[index] = resolveLayer(layer)
                }
            }
            if request.results.contains(where: { $0.status == .unpublished }) {
                textureRequests.append(request)
            } else { completed.append(request) }
        }
        for request in completed { request.completion(request.results) }
        let credits = consumptionRequests
        consumptionRequests.removeAll()
        for credit in credits where credit.generation == token {
            if consumed(credit.layers) { credit.completion() }
            else { consumptionRequests.append(credit) }
        }
        if let resourceID { callback?(resourceID) }
    }

    // Called with lock held. One run-loop wakeup covers a batch of decoded
    // frames, including native resize/tracking modes.
    nonisolated private func scheduleNotification() {
        guard !notificationScheduled else { return }
        notificationScheduled = true
        MainRunLoop.perform { [weak self] in self?.deliverAvailableFrames() }
    }

    /// Returns immediately. Under load, media/network work cannot starve the
    /// AppKit event loop or native live-resize callbacks.
    @discardableResult
    nonisolated public func ingest(header: MediaWire.Header, payload: Data) -> Bool {
        guard header.width > 0, header.height > 0,
              payload.count == Int(header.payloadLength), payload.count <= MediaWire.maximumPayloadSize else { return false }
        lock.lock()
        guard !decodingFailed, payload.count <= 128 * 1024 * 1024 - queuedMediaBytes else {
            lock.unlock()
            return false // Never silently drop a compressed reference frame.
        }
        let token = generation
        queuedMediaBytes += payload.count
        lock.unlock()
        decodeQueue.async { [weak self] in
            guard let self else { return }
            defer {
                self.lock.lock()
                self.queuedMediaBytes -= payload.count
                self.lock.unlock()
            }
            let id = header.surfaceID
            self.lock.lock()
            guard self.generation == token, !self.decodingFailed, !self.destroyedSurfaces.contains(id) else { self.lock.unlock(); return }
            let stream: Stream
            if let existing = self.streams[id] {
                stream = existing
            } else {
                stream = Stream(generation: token)
                self.streams[id] = stream
                stream.decoder.onFrame = { [weak self, weak stream] resourceID, pixelBuffer in
                    guard let self, let stream else { return }
                    self.receiveDecoded(pixelBuffer, stream: stream,
                        surfaceID: id, resourceID: resourceID)
                }
                stream.decoder.onFailure = { [weak self, weak stream] resourceID in
                    guard let self, let stream else { return }
                    self.lock.lock()
                    let current = self.streams[id] === stream && stream.pendingIDs.contains(resourceID)
                    self.lock.unlock()
                    if current { self.fail("Could not decode remote frame \(resourceID).", generation: stream.generation) }
                }
            }
            stream.av1.onFrame = stream.decoder.onFrame
            stream.av1.onFailure = stream.decoder.onFailure
            if header.bitstreamEpoch != 0, header.bitstreamEpoch != stream.epoch {
                for resourceID in stream.resourceIDs {
                    self.frames.removeValue(forKey: resourceID)
                }
                stream.resourceIDs.removeAll(keepingCapacity: true)
                stream.pendingIDs.removeAll(keepingCapacity: true)
                stream.expectedAlpha.removeAll(keepingCapacity: true)
                stream.alphaPlanes.removeAll(keepingCapacity: true)
                stream.lastAlpha = nil
                stream.pendingVideo.removeAll(keepingCapacity: true)
                stream.epoch = header.bitstreamEpoch
                stream.codec = nil
                self.lock.unlock()
                stream.reset()
            } else {
                self.lock.unlock()
            }

            switch header.codec {
            case .h264, .av1:
                self.lock.lock()
                // A codec switch must be accompanied by a new reference epoch.
                if let codec = stream.codec, codec != header.codec {
                    stream.codec = header.codec
                    self.lock.unlock()
                    self.fail("Remote codec changed without a new epoch.", generation: token)
                    return
                }
                stream.codec = header.codec
                if header.flags & MediaWire.flagHasAlpha != 0 {
                    if header.flags & MediaWire.flagReuseAlpha != 0 {
                        guard let alpha = stream.lastAlpha, alpha.width == Int(header.width),
                              alpha.height == Int(header.height) else {
                            self.lock.unlock()
                            self.fail("Invalid remote alpha reuse reference.", generation: token)
                            return
                        }
                        stream.alphaPlanes[header.resourceID] = alpha
                    }
                    stream.expectedAlpha.insert(header.resourceID)
                } else {
                    stream.expectedAlpha.remove(header.resourceID)
                    stream.alphaPlanes.removeValue(forKey: header.resourceID)
                }
                self.trackPending(header.resourceID, stream: stream)
                self.lock.unlock()
                if header.codec == .av1 {
                    stream.av1.decode(obu: payload, width: Int(header.width), height: Int(header.height),
                        bitstreamEpoch: header.bitstreamEpoch, resourceID: header.resourceID)
                } else { stream.decoder.decode(
                    annexB: payload,
                    width: Int(header.width),
                    height: Int(header.height),
                    bitstreamEpoch: header.bitstreamEpoch,
                    resourceID: header.resourceID,
                    requireHardware: header.flags & MediaWire.flagHardwareEncoded != 0) }
            case .alphaRLE:
                let width = Int(header.width)
                let height = Int(header.height)
                guard width <= Int.max / height,
                      let alpha = MediaWire.decodeAlphaRLE(
                        payload, pixelCount: width * height) else {
                    self.fail("Malformed remote alpha sidecar.", generation: token)
                    return
                }
                self.receiveAlpha(
                    AlphaPlane(width: width, height: height, bytes: alpha),
                    stream: stream, surfaceID: id, resourceID: header.resourceID)
            }
        }
        return true
    }

    nonisolated public func removeSurface(_ surfaceID: UInt32) {
        decodeQueue.async { [weak self] in
            guard let self else { return }
            self.lock.lock()
            self.destroyedSurfaces.insert(surfaceID)
            let stream = self.streams.removeValue(forKey: surfaceID)
            if let stream {
                for resourceID in stream.resourceIDs {
                    self.frames.removeValue(forKey: resourceID)
                }
            }
            self.scheduleNotification()
            self.lock.unlock()
            stream?.reset()
        }
    }

    nonisolated public func removeAll() {
        lock.lock()
        generation &+= 1
        decodingFailed = false
        hardwareFrames = 0; softwareFrames = 0
        frames.removeAll()
        newestNotification = nil
        scheduleNotification()
        // Queue reset while admission is locked: a new-generation ingest must
        // never run before reset and accidentally reuse an old Stream object.
        decodeQueue.async { [weak self] in
            guard let self else { return }
            self.lock.lock()
            let removed = Array(self.streams.values)
            self.streams.removeAll()
            self.frames.removeAll()
            self.destroyedSurfaces.removeAll()
            self.lock.unlock()
            for stream in removed { stream.reset() }
        }
        lock.unlock()
    }

    nonisolated private func publish(
        _ pixelBuffer: CVPixelBuffer, stream: Stream,
        surfaceID: UInt32, resourceID: UInt32, alpha: AlphaPlane? = nil
    ) {
        let ioSurface: IOSurfaceRef?
        var pixelOwner: CVPixelBuffer?
        if let alpha {
            ioSurface = Self.copyToIOSurface(pixelBuffer, alpha: alpha)
        } else if let decoded = CVPixelBufferGetIOSurface(pixelBuffer)?.takeUnretainedValue() {
            ioSurface = decoded
            pixelOwner = pixelBuffer
        } else {
            ioSurface = Self.copyToIOSurface(pixelBuffer)
        }
        guard let ioSurface else {
            fail("Could not construct remote frame pixels.", generation: stream.generation)
            return
        }

        lock.lock()
        guard streams[surfaceID] === stream, stream.generation == generation, !decodingFailed else {
            lock.unlock()
            return
        }
        if let latest = stream.publishedThrough, Int32(bitPattern: latest &- resourceID) > 0 {
            lock.unlock(); return
        }
        stream.publishedThrough = resourceID
        frames[resourceID] = StoredFrame(
            surface: ioSurface, surfaceID: surfaceID, pixelBuffer: pixelOwner)
        stream.pendingIDs.removeAll { $0 == resourceID }
        stream.resourceIDs.removeAll { $0 == resourceID }
        stream.resourceIDs.append(resourceID)
        while stream.resourceIDs.count > maximumFramesPerStream {
            frames.removeValue(forKey: stream.resourceIDs.removeFirst())
        }
        newestNotification = resourceID
        scheduleNotification()
        lock.unlock()
    }

    nonisolated private func receiveDecoded(
        _ pixelBuffer: CVPixelBuffer, stream: Stream,
        surfaceID: UInt32, resourceID: UInt32
    ) {
        lock.lock()
        guard streams[surfaceID] === stream, stream.generation == generation, stream.pendingIDs.contains(resourceID) else {
            lock.unlock()
            return
        }
        if stream.codec == .h264 ? stream.decoder.usingHardware : stream.av1.usingHardware {
            hardwareFrames += 1
        } else { softwareFrames += 1 }
        if stream.expectedAlpha.contains(resourceID),
           stream.alphaPlanes[resourceID] == nil {
            stream.pendingVideo[resourceID] = pixelBuffer
            trackPending(resourceID, stream: stream)
            lock.unlock()
            return
        }
        let alpha = stream.alphaPlanes.removeValue(forKey: resourceID)
        stream.expectedAlpha.remove(resourceID)
        stream.pendingVideo.removeValue(forKey: resourceID)
        lock.unlock()
        publish(
            pixelBuffer, stream: stream, surfaceID: surfaceID,
            resourceID: resourceID, alpha: alpha)
    }

    nonisolated private func receiveAlpha(
        _ alpha: AlphaPlane, stream: Stream,
        surfaceID: UInt32, resourceID: UInt32
    ) {
        lock.lock()
        guard streams[surfaceID] === stream, stream.generation == generation else {
            lock.unlock()
            return
        }
        stream.alphaPlanes[resourceID] = alpha
        stream.lastAlpha = alpha
        trackPending(resourceID, stream: stream)
        let video = stream.pendingVideo.removeValue(forKey: resourceID)
        if video != nil { stream.expectedAlpha.remove(resourceID) }
        lock.unlock()
        if let video {
            publish(
                video, stream: stream, surfaceID: surfaceID,
                resourceID: resourceID, alpha: alpha)
        }
    }

    nonisolated private func trackPending(_ resourceID: UInt32, stream: Stream) {
        stream.pendingIDs.removeAll { $0 == resourceID }
        stream.pendingIDs.append(resourceID)
        while stream.pendingIDs.count > maximumFramesPerStream * 2 {
            let expired = stream.pendingIDs.removeFirst()
            stream.alphaPlanes.removeValue(forKey: expired)
            stream.pendingVideo.removeValue(forKey: expired)
            stream.expectedAlpha.remove(expired)
        }
    }

    nonisolated private static func copyToIOSurface(
        _ pixelBuffer: CVPixelBuffer, alpha: AlphaPlane? = nil
    ) -> IOSurfaceRef? {
        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)
        let bytesPerRow = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, width * 4)
        guard let surface = IOSurface(properties: [
            .width: width,
            .height: height,
            .bytesPerElement: 4,
            .bytesPerRow: bytesPerRow,
            .pixelFormat: kCVPixelFormatType_32BGRA,
        ]) else { return nil }
        let ref = surface as IOSurfaceRef
        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
        guard let src = CVPixelBufferGetBaseAddress(pixelBuffer) else { return nil }
        let srcRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
        IOSurfaceLock(ref, [], nil)
        defer { IOSurfaceUnlock(ref, [], nil) }
        let dst = IOSurfaceGetBaseAddress(ref)
        if let alpha {
            guard alpha.width == width, alpha.height == height,
                  alpha.bytes.count == width * height else { return nil }
            let status = alpha.bytes.withUnsafeBytes { raw -> vImage_Error in
                var plane = vImage_Buffer(data: UnsafeMutableRawPointer(mutating: raw.baseAddress!),
                    height: vImagePixelCount(height), width: vImagePixelCount(width), rowBytes: width)
                var source = vImage_Buffer(data: src, height: vImagePixelCount(height),
                    width: vImagePixelCount(width), rowBytes: srcRow)
                var target = vImage_Buffer(data: dst, height: vImagePixelCount(height),
                    width: vImagePixelCount(width), rowBytes: bytesPerRow)
                // BGRA alpha is the last component (mask 1). Copy RGB and
                // inject alpha in one native pass, respecting both row strides.
                return vImageOverwriteChannels_ARGB8888(&plane, &source, &target, 1, vImage_Flags(kvImageNoFlags))
            }
            guard status == kvImageNoError else { return nil }
        } else {
            for row in 0..<height {
                memcpy(dst.advanced(by: row * bytesPerRow), src.advanced(by: row * srcRow), width * 4)
            }
        }
        return ref
    }
}
