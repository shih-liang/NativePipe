import CoreVideo
import Foundation
import IOSurface
import NativePipeProtocol
import NativePipeWindowing

/// Thread-safe remote frame store. H.264 parsing and VideoToolbox submission
/// stay on `decodeQueue`; only the finished IOSurface is observed by AppKit.
public final class RemoteFrameSource: @unchecked Sendable, FrameSource {
    private final class Stream {
        let decoder = H264Decoder()
        var surface: IOSurfaceRef?
        var epoch: UInt16 = 0
    }

    private let lock = NSLock()
    private let decodeQueue = DispatchQueue(
        label: "com.nativepipe.remote.decode", qos: .userInteractive)
    nonisolated(unsafe) private var streams: [UInt32: Stream] = [:]
    nonisolated(unsafe) private var frameAvailable: (@MainActor (UInt32) -> Void)?

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
        return streams[resourceID]?.surface
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
                stream.decoder.onFrame = { [weak self, weak stream] pixelBuffer in
                    guard let self, let stream else { return }
                    self.publish(pixelBuffer, stream: stream, id: id)
                }
            }
            if header.bitstreamEpoch != 0, header.bitstreamEpoch != stream.epoch {
                stream.surface = nil
                stream.epoch = header.bitstreamEpoch
                self.lock.unlock()
                stream.decoder.reset()
            } else {
                self.lock.unlock()
            }

            stream.decoder.decode(
                annexB: payload,
                width: Int(header.width),
                height: Int(header.height),
                bitstreamEpoch: header.bitstreamEpoch)
        }
    }

    nonisolated public func removeSurface(_ surfaceID: UInt32) {
        decodeQueue.async { [weak self] in
            guard let self else { return }
            self.lock.lock()
            let stream = self.streams.removeValue(forKey: surfaceID)
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
            self.lock.unlock()
            for stream in removed { stream.decoder.reset() }
        }
    }

    nonisolated private func publish(_ pixelBuffer: CVPixelBuffer, stream: Stream, id: UInt32) {
        let ioSurface: IOSurfaceRef?
        if let decoded = CVPixelBufferGetIOSurface(pixelBuffer)?.takeUnretainedValue() {
            ioSurface = decoded
        } else {
            ioSurface = Self.copyToIOSurface(pixelBuffer)
        }
        guard let ioSurface else { return }

        lock.lock()
        guard streams[id] === stream else {
            lock.unlock()
            return
        }
        stream.surface = ioSurface
        let callback = frameAvailable
        lock.unlock()
        if let callback {
            Task { @MainActor in callback(id) }
        }
    }

    nonisolated private static func copyToIOSurface(_ pixelBuffer: CVPixelBuffer) -> IOSurfaceRef? {
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
        return ref
    }
}
