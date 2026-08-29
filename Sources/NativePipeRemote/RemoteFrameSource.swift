import CoreVideo
import Foundation
import IOSurface
import Metal
import NativePipeProtocol
import NativePipeWindowing

/// Thread-safe remote frame store. H.264 parsing and VideoToolbox submission
/// stay on `decodeQueue`; only the finished IOSurface is observed by AppKit.
public final class RemoteFrameSource: @unchecked Sendable, FrameSource {
    private final class Stream {
        let decoder = H264Decoder()
        var epoch: UInt16 = 0
        var resourceIDs: [UInt32] = []
        var pendingIDs: [UInt32] = []
        var expectedAlpha: Set<UInt32> = []
        var alphaPlanes: [UInt32: AlphaPlane] = [:]
        var pendingVideo: [UInt32: CVPixelBuffer] = [:]
    }

    private struct AlphaPlane {
        let width: Int
        let height: Int
        let bytes: Data
    }

    private final class StoredFrame {
        let surface: IOSurfaceRef
        var texture: MTLTexture?

        init(surface: IOSurfaceRef) {
            self.surface = surface
        }
    }

    private let lock = NSLock()
    private let decodeQueue = DispatchQueue(
        label: "com.nativepipe.remote.decode", qos: .userInteractive)
    nonisolated(unsafe) private var streams: [UInt32: Stream] = [:]
    nonisolated(unsafe) private var frames: [UInt32: StoredFrame] = [:]
    nonisolated(unsafe) private var frameAvailable: (@MainActor (UInt32) -> Void)?
    private let device = MTLCreateSystemDefaultDevice()
    private let maximumFramesPerStream = 12

    public init() {}

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
        lock.lock()
        guard let frame = frames[resourceID] else {
            lock.unlock()
            return nil
        }
        if let texture = frame.texture {
            lock.unlock()
            return texture
        }
        guard let device,
              width > 0, height > 0,
              IOSurfaceGetWidth(frame.surface) >= width,
              IOSurfaceGetHeight(frame.surface) >= height else {
            lock.unlock()
            return nil
        }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height,
            mipmapped: false)
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderRead]
        let texture = device.makeTexture(
            descriptor: descriptor, iosurface: frame.surface, plane: 0)
        frame.texture = texture
        lock.unlock()
        return texture
    }

    @MainActor
    public func metalTextures(
        for layers: [Windowing.SceneLayer],
        completion: @escaping @MainActor ([FrameTextureResolution]) -> Void
    ) {
        completion(layers.map { layer in
            if let texture = metalTexture(
                forResource: layer.resourceID,
                width: layer.width, height: layer.height,
                bytesPerRow: layer.bytesPerRow, format: 1)
            {
                return FrameTextureResolution(status: .ready, texture: texture)
            }
            return FrameTextureResolution(
                status: isResourcePublished(layer.resourceID)
                    ? .unavailable : .unpublished)
        })
    }

    /// Returns immediately. Under load, media/network work cannot starve the
    /// AppKit event loop or native live-resize callbacks.
    nonisolated public func ingest(header: MediaWire.Header, payload: Data) {
        decodeQueue.async { [weak self] in
            guard let self else { return }
            let id = header.surfaceID
            self.lock.lock()
            let stream: Stream
            if let existing = self.streams[id] {
                stream = existing
            } else {
                stream = Stream()
                self.streams[id] = stream
                stream.decoder.onFrame = { [weak self, weak stream] resourceID, pixelBuffer in
                    guard let self, let stream else { return }
                    self.decodeQueue.async { [weak self, weak stream] in
                        guard let self, let stream else { return }
                        self.receiveDecoded(
                            pixelBuffer, stream: stream,
                            surfaceID: id, resourceID: resourceID)
                    }
                }
            }
            if header.bitstreamEpoch != 0, header.bitstreamEpoch != stream.epoch {
                for resourceID in stream.resourceIDs {
                    self.frames.removeValue(forKey: resourceID)
                }
                stream.resourceIDs.removeAll(keepingCapacity: true)
                stream.pendingIDs.removeAll(keepingCapacity: true)
                stream.expectedAlpha.removeAll(keepingCapacity: true)
                stream.alphaPlanes.removeAll(keepingCapacity: true)
                stream.pendingVideo.removeAll(keepingCapacity: true)
                stream.epoch = header.bitstreamEpoch
                self.lock.unlock()
                stream.decoder.reset()
            } else {
                self.lock.unlock()
            }

            switch header.codec {
            case .h264:
                self.lock.lock()
                if header.flags & MediaWire.flagHasAlpha != 0 {
                    stream.expectedAlpha.insert(header.resourceID)
                } else {
                    stream.expectedAlpha.remove(header.resourceID)
                    stream.alphaPlanes.removeValue(forKey: header.resourceID)
                }
                self.trackPending(header.resourceID, stream: stream)
                self.lock.unlock()
                stream.decoder.decode(
                    annexB: payload,
                    width: Int(header.width),
                    height: Int(header.height),
                    bitstreamEpoch: header.bitstreamEpoch,
                    resourceID: header.resourceID)
            case .alphaRLE:
                let width = Int(header.width)
                let height = Int(header.height)
                guard width <= Int.max / height,
                      let alpha = MediaWire.decodeAlphaRLE(
                        payload, pixelCount: width * height) else {
                    fputs("remotepipe: rejected malformed alpha sidecar\n", stderr)
                    return
                }
                self.receiveAlpha(
                    AlphaPlane(width: width, height: height, bytes: alpha),
                    stream: stream, surfaceID: id, resourceID: header.resourceID)
            }
        }
    }

    nonisolated public func removeSurface(_ surfaceID: UInt32) {
        decodeQueue.async { [weak self] in
            guard let self else { return }
            self.lock.lock()
            let stream = self.streams.removeValue(forKey: surfaceID)
            if let stream {
                for resourceID in stream.resourceIDs {
                    self.frames.removeValue(forKey: resourceID)
                }
            }
            self.lock.unlock()
            stream?.decoder.reset()
        }
    }

    nonisolated public func removeAll() {
        decodeQueue.async { [weak self] in
            guard let self else { return }
            self.lock.lock()
            let removed = Array(self.streams.values)
            self.streams.removeAll()
            self.frames.removeAll()
            self.lock.unlock()
            for stream in removed { stream.decoder.reset() }
        }
    }

    nonisolated private func publish(
        _ pixelBuffer: CVPixelBuffer, stream: Stream,
        surfaceID: UInt32, resourceID: UInt32, alpha: AlphaPlane? = nil
    ) {
        let ioSurface: IOSurfaceRef?
        if let alpha {
            ioSurface = Self.copyToIOSurface(pixelBuffer, alpha: alpha)
        } else if let decoded = CVPixelBufferGetIOSurface(pixelBuffer)?.takeUnretainedValue() {
            ioSurface = decoded
        } else {
            ioSurface = Self.copyToIOSurface(pixelBuffer)
        }
        guard let ioSurface else { return }

        lock.lock()
        guard streams[surfaceID] === stream else {
            lock.unlock()
            return
        }
        frames[resourceID] = StoredFrame(surface: ioSurface)
        stream.resourceIDs.removeAll { $0 == resourceID }
        stream.resourceIDs.append(resourceID)
        while stream.resourceIDs.count > maximumFramesPerStream {
            frames.removeValue(forKey: stream.resourceIDs.removeFirst())
        }
        let callback = frameAvailable
        lock.unlock()
        if let callback {
            Task { @MainActor in callback(resourceID) }
        }
    }

    nonisolated private func receiveDecoded(
        _ pixelBuffer: CVPixelBuffer, stream: Stream,
        surfaceID: UInt32, resourceID: UInt32
    ) {
        lock.lock()
        guard streams[surfaceID] === stream else {
            lock.unlock()
            return
        }
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
        guard streams[surfaceID] === stream else {
            lock.unlock()
            return
        }
        stream.alphaPlanes[resourceID] = alpha
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
        guard let src = CVPixelBufferGetBaseAddress(pixelBuffer) else { return ref }
        let srcRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
        IOSurfaceLock(ref, [], nil)
        defer { IOSurfaceUnlock(ref, [], nil) }
        let dst = IOSurfaceGetBaseAddress(ref)
        let copyWidth = min(srcRow, bytesPerRow)
        for row in 0..<height {
            memcpy(dst.advanced(by: row * bytesPerRow), src.advanced(by: row * srcRow), copyWidth)
        }
        if let alpha,
           alpha.width <= width, alpha.height <= height,
           alpha.bytes.count == alpha.width * alpha.height {
            alpha.bytes.withUnsafeBytes { raw in
                guard let source = raw.bindMemory(to: UInt8.self).baseAddress else { return }
                let pixels = dst.bindMemory(to: UInt8.self, capacity: bytesPerRow * height)
                for row in 0..<alpha.height {
                    let output = pixels.advanced(by: row * bytesPerRow)
                    let input = source.advanced(by: row * alpha.width)
                    for column in 0..<alpha.width {
                        output[column * 4 + 3] = input[column]
                    }
                }
            }
        }
        return ref
    }
}
