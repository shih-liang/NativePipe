import CNativePipeAV1
import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

/// One low-overhead AV1 temporal unit per immutable resource. All state lives
/// on RemoteFrameSource.decodeQueue; neither backend buffers future frames.
final class AV1Decoder {
    private var session: VTDecompressionSession?
    private var format: CMVideoFormatDescription?
    private var software: OpaquePointer?
    private var epoch: UInt16 = 0
    private var dimensions = (0, 0)
    private var replay: [Data] = []
    private var replayBytes = 0
    private let allowHardware: Bool
    private(set) var usingHardware = false
    var onFrame: ((UInt32, CVPixelBuffer) -> Void)?
    var onFailure: ((UInt32) -> Void)?

    init(allowHardware: Bool = true) { self.allowHardware = allowHardware }
    deinit { reset() }

    func reset() {
        if let session { VTDecompressionSessionInvalidate(session) }
        session = nil; format = nil
        np_av1_decoder_destroy(software); software = nil
        epoch = 0; dimensions = (0, 0); usingHardware = false
        replay.removeAll(); replayBytes = 0
    }

    /// Also validates OBU framing before passing untrusted bytes to either decoder.
    static func sequenceHeader(in data: Data) throws -> Data? {
        try data.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) -> Data? in
            var position = 0, sequence: Data?, obuCount = 0, frameCount = 0
            while position < bytes.count {
                obuCount += 1
                guard obuCount <= 128 else { throw RemoteError.message("Too many AV1 OBUs.") }
                let start = position, header = bytes[position]
                position += 1
                guard header & 0x81 == 0, header & 2 != 0 else { throw RemoteError.message("Invalid AV1 OBU header.") }
                if header & 4 != 0 {
                    guard position < bytes.count, bytes[position] == 0 else { throw RemoteError.message("Unsupported AV1 layer.") }
                    position += 1
                }
                var size: UInt64 = 0, finished = false
                for shift in stride(from: 0, to: 56, by: 7) {
                    guard position < bytes.count else { throw RemoteError.message("Truncated AV1 OBU size.") }
                    let value = bytes[position]; position += 1
                    size |= UInt64(value & 127) << shift
                    if value & 128 == 0 { finished = true; break }
                }
                guard finished, size <= UInt64(bytes.count - position) else { throw RemoteError.message("Invalid AV1 OBU size.") }
                position += Int(size)
                let type = (header >> 3) & 15
                if type == 1 { sequence = data.subdata(in: start..<position) }
                if type == 3 || type == 6 { frameCount += 1 }
                guard frameCount <= 1 else { throw RemoteError.message("Multiple AV1 frames for one resource.") }
            }
            return sequence
        }
    }

    func decode(obu: Data, width: Int, height: Int, bitstreamEpoch: UInt16, resourceID: UInt32) {
        guard width > 0, height > 0, width <= 8192, height <= 8192,
              width * height <= 16_777_216, !obu.isEmpty, obu.count <= 32 * 1024 * 1024 else {
            onFailure?(resourceID); return
        }
        if epoch != bitstreamEpoch || dimensions != (width, height) {
            reset(); epoch = bitstreamEpoch; dimensions = (width, height)
        }
        do {
            if let sequence = try Self.sequenceHeader(in: obu) {
                var config = [UInt8](repeating: 0, count: 4)
                let valid = sequence.withUnsafeBytes {
                    np_av1_configuration($0.bindMemory(to: UInt8.self).baseAddress, $0.count,
                        Int32(width), Int32(height), &config)
                }
                guard valid == 0 else { throw RemoteError.message("Unsupported AV1 sequence.") }
                // Encoders emit a sequence header at every random-access keyframe.
                replay.removeAll(keepingCapacity: true); replayBytes = 0
                if software == nil, session == nil {
                    if allowHardware { createHardware(config: Data(config) + sequence, width: width, height: height) }
                    if session == nil { software = np_av1_decoder_create() }
                }
            }
            var pixel: CVPixelBuffer?
            if session != nil {
                // Bounded compressed GOP permits software recovery after a driver
                // failure, without publishing replays or retaining decoded pixels.
                if replay.count < 120, replayBytes <= 32 * 1024 * 1024 - obu.count {
                    pixel = decodeHardware(obu)
                }
                if let pixel, CVPixelBufferGetWidth(pixel) == width, CVPixelBufferGetHeight(pixel) == height {
                    replay.append(obu); replayBytes += obu.count
                    onFrame?(resourceID, pixel); return
                }
                if let session { VTDecompressionSessionInvalidate(session) }
                session = nil; usingHardware = false
                software = np_av1_decoder_create()
                for previous in replay {
                    guard decodeSoftware(previous, width: width, height: height) != nil else {
                        throw RemoteError.message("Could not recover AV1 reference frames.")
                    }
                }
                replay.removeAll(); replayBytes = 0
            }
            pixel = decodeSoftware(obu, width: width, height: height)
            guard let pixel else { throw RemoteError.message("Could not decode AV1 frame.") }
            onFrame?(resourceID, pixel)
        } catch { onFailure?(resourceID) }
    }

    private func decodeSoftware(_ packet: Data, width: Int, height: Int) -> CVPixelBuffer? {
        guard let software else { return nil }
        return packet.withUnsafeBytes {
            np_av1_decoder_decode(software, $0.bindMemory(to: UInt8.self).baseAddress,
                $0.count, Int32(width), Int32(height))
        }
    }

    private func createHardware(config: Data, width: Int, height: Int) {
        let extensions: [CFString: Any] = [
            kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: ["av1C": config]]
        guard CMVideoFormatDescriptionCreate(allocator: nil, codecType: kCMVideoCodecType_AV1,
            width: Int32(width), height: Int32(height), extensions: extensions as CFDictionary,
            formatDescriptionOut: &format) == noErr, let format else { return }
        let specification = [kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: true] as CFDictionary
        let attrs: [CFString: Any] = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferIOSurfacePropertiesKey: [:]]
        if VTDecompressionSessionCreate(allocator: nil, formatDescription: format,
            decoderSpecification: specification, imageBufferAttributes: attrs as CFDictionary,
            outputCallback: nil, decompressionSessionOut: &session) == noErr, let session {
            usingHardware = true
            VTSessionSetProperty(session, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        }
    }

    private final class Result: @unchecked Sendable {
        // VT invokes this before the synchronous decode returns (both flags clear).
        var pixel: CVPixelBuffer?
    }

    private func decodeHardware(_ packet: Data) -> CVPixelBuffer? {
        guard let session, let format else { return nil }
        var block: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: nil,
            blockLength: packet.count, blockAllocator: nil, customBlockSource: nil,
            offsetToData: 0, dataLength: packet.count, flags: 0, blockBufferOut: &block) == noErr,
            let block else { return nil }
        let copied = packet.withUnsafeBytes {
            CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block,
                offsetIntoDestination: 0, dataLength: $0.count)
        }
        var size = packet.count, sample: CMSampleBuffer?
        guard copied == noErr, CMSampleBufferCreateReady(allocator: nil, dataBuffer: block,
            formatDescription: format, sampleCount: 1, sampleTimingEntryCount: 0,
            sampleTimingArray: nil, sampleSizeEntryCount: 1, sampleSizeArray: &size,
            sampleBufferOut: &sample) == noErr, let sample else { return nil }
        let result = Result()
        let status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: sample,
            flags: [], infoFlagsOut: nil) { status, _, pixel, _, _ in
                if status == noErr { result.pixel = pixel }
            }
        return status == noErr ? result.pixel : nil
    }
}
