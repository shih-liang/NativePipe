import CoreVideo
import Foundation
import IOSurface
import NativePipeProtocol
import NativePipeWindowing

/// `FrameSource` backed by VideoToolbox decode of NPEN H.264 frames.
///
/// `resourceID` on committed encoded frames is the remote surface id (stream
/// key), not a virtio-gpu resource.
@MainActor
public final class RemoteFrameSource: FrameSource {
    private final class Stream {
        let decoder = H264Decoder()
        var surface: IOSurfaceRef?
        var epoch: UInt16 = 0
    }

    private var streams: [UInt32: Stream] = [:]

    public init() {}

    public func surface(forResource resourceID: UInt32) -> IOSurfaceRef? {
        streams[resourceID]?.surface
    }

    public func ingest(header: MediaWire.Header, payload: Data) {
        let id = header.surfaceID
        let stream = streams[id] ?? {
            let created = Stream()
            streams[id] = created
            return created
        }()

        if header.bitstreamEpoch != 0, header.bitstreamEpoch != stream.epoch {
            stream.decoder.reset()
            stream.surface = nil
            stream.epoch = header.bitstreamEpoch
        }

        stream.decoder.decode(
            annexB: payload,
            width: Int(header.width),
            height: Int(header.height),
            bitstreamEpoch: header.bitstreamEpoch)

        if stream.surface == nil {
            fputs(
                "nativepipe-remote: decode miss surface=\(id) bytes=\(payload.count) "
                    + "prefix=\(payload.prefix(16).map { String(format: "%02x", $0) }.joined(separator: " ")) "
                    + "epoch=\(header.bitstreamEpoch)\n",
                stderr)
            fflush(stderr)
        }

        guard let pixelBuffer = stream.decoder.latestPixelBuffer else { return }
        if let ioSurface = CVPixelBufferGetIOSurface(pixelBuffer)?.takeUnretainedValue() {
            stream.surface = ioSurface
            return
        }
        // Fallback: copy into a dedicated IOSurface if VT did not back the
        // buffer with one (unusual with our imageBufferAttributes).
        stream.surface = Self.copyToIOSurface(pixelBuffer)
    }

    public func removeSurface(_ surfaceID: UInt32) {
        streams[surfaceID]?.decoder.reset()
        streams.removeValue(forKey: surfaceID)
    }

    public func removeAll() {
        for stream in streams.values { stream.decoder.reset() }
        streams.removeAll()
    }

    private static func copyToIOSurface(_ pixelBuffer: CVPixelBuffer) -> IOSurfaceRef? {
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
        let rows = height
        let copyWidth = min(srcRow, bytesPerRow)
        for row in 0..<rows {
            memcpy(dst.advanced(by: row * bytesPerRow), src.advanced(by: row * srcRow), copyWidth)
        }
        return ref
    }
}
