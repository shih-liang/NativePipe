import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

/// Incremental H.264 Annex-B → CVPixelBuffer decoder (VideoToolbox).
final class H264Decoder {
    // With no asynchronous flag, VT completes the output handler before
    // DecodeFrame returns. All state and delivery stay on the decode queue.
    private final class DecodeResult: @unchecked Sendable {
        var pixel: CVPixelBuffer?
        var status: OSStatus = -1
    }
    static var hardwareAvailable: Bool { VTIsHardwareDecodeSupported(kCMVideoCodecType_H264) }
    private(set) var usingHardware = false
    private var requiresHardware = false

    private var session: VTDecompressionSession?
    private var formatDescription: CMVideoFormatDescription?
    private var sps: Data?
    private var pps: Data?
    private var width: Int = 0
    private var height: Int = 0
    private var epoch: UInt16 = 0

    var onFrame: ((UInt32, CVPixelBuffer) -> Void)?
    var onFailure: ((UInt32) -> Void)?

    func reset() {
        if let session {
            VTDecompressionSessionInvalidate(session)
        }
        session = nil
        usingHardware = false
        formatDescription = nil
        sps = nil
        pps = nil
        epoch = 0
    }

    deinit { reset() }

    func decode(
        annexB: Data, width: Int, height: Int,
        bitstreamEpoch: UInt16, resourceID: UInt32, requireHardware: Bool = false
    ) {
        guard width >= 2, height >= 2, width <= 8192, height <= 8192,
              width * height <= 16_777_216, annexB.count <= 32 * 1024 * 1024 else {
            onFailure?(resourceID); return
        }
        if (bitstreamEpoch != 0 && bitstreamEpoch != epoch) ||
           width != self.width || height != self.height || requireHardware != requiresHardware {
            reset()
            epoch = bitstreamEpoch
        }
        requiresHardware = requireHardware
        self.width = width
        self.height = height

        let nals = Self.splitAnnexB(annexB)
        guard !nals.isEmpty else {
            fputs("nativepipe-remote: H264: 0 NALs in \(annexB.count) bytes\n", stderr)
            fflush(stderr)
            onFailure?(resourceID)
            return
        }

        var vcl: [Data] = []
        var formatChanged = false
        for nal in nals {
            let type = nal[nal.startIndex] & 0x1f
            switch type {
            case 7:
                let value = Data(nal)
                if value != sps { sps = value; formatChanged = true }
            case 8:
                let value = Data(nal)
                if value != pps { pps = value; formatChanged = true }
            case 1, 5:
                vcl.append(Data(nal))
            default:
                break
            }
        }
        if formatChanged, !rebuildFormatIfPossible() { onFailure?(resourceID); return }
        guard !vcl.isEmpty else { onFailure?(resourceID); return }
        guard session != nil else {
            fputs("nativepipe-remote: H264: VCL without VT session\n", stderr)
            fflush(stderr)
            onFailure?(resourceID)
            return
        }
        if !decodeAccessUnit(vcl, resourceID: resourceID) { onFailure?(resourceID) }
    }

    private func rebuildFormatIfPossible() -> Bool {
        guard let sps, let pps else { return false }
        var description: CMVideoFormatDescription?
        let status: OSStatus = sps.withUnsafeBytes { spsRaw in
            guard let spsBase = spsRaw.bindMemory(to: UInt8.self).baseAddress else { return -1 }
            return pps.withUnsafeBytes { ppsRaw in
                guard let ppsBase = ppsRaw.bindMemory(to: UInt8.self).baseAddress else { return -1 }
                var pointers: [UnsafePointer<UInt8>] = [spsBase, ppsBase]
                var sizes: [Int] = [sps.count, pps.count]
                return pointers.withUnsafeMutableBufferPointer { pointerBuffer in
                    sizes.withUnsafeMutableBufferPointer { sizeBuffer in
                        CMVideoFormatDescriptionCreateFromH264ParameterSets(
                            allocator: kCFAllocatorDefault,
                            parameterSetCount: 2,
                            parameterSetPointers: pointerBuffer.baseAddress!,
                            parameterSetSizes: sizeBuffer.baseAddress!,
                            nalUnitHeaderLength: 4,
                            formatDescriptionOut: &description)
                    }
                }
            }
        }
        if status != noErr {
            fputs("nativepipe-remote: H264 format desc failed status=\(status)\n", stderr)
            fflush(stderr)
            reset(); return false
        }
        guard let description else { reset(); return false }
        let dimensions = CMVideoFormatDescriptionGetDimensions(description)
        guard dimensions.width == width, dimensions.height == height else {
            reset(); return false
        }
        formatDescription = description
        recreateSession(description)
        return session != nil
    }

    private func recreateSession(_ description: CMVideoFormatDescription) {
        if let session {
            VTDecompressionSessionInvalidate(session)
            self.session = nil
        }
        usingHardware = false
        var session: VTDecompressionSession?
        let attrs: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
        ]
        let status = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            formatDescription: description,
            decoderSpecification: [requiresHardware
                ? kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder
                : kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: true] as CFDictionary,
            imageBufferAttributes: attrs as CFDictionary,
            outputCallback: nil,
            decompressionSessionOut: &session)
        if status == noErr, let session {
            // VTSessionCopyProperty uses an untyped out pointer and returns
            // a retained CF object; make that ownership explicit to Swift.
            var property: Unmanaged<CFTypeRef>?
            let checked = VTSessionCopyProperty(session,
                key: kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                allocator: nil, valueOut: &property)
            let hardware = property?.takeRetainedValue() as? NSNumber
            usingHardware = checked == noErr && hardware?.boolValue == true
            guard !requiresHardware || usingHardware else {
                VTDecompressionSessionInvalidate(session)
                fputs("nativepipe-remote: H264 hardware decoder is unavailable\n", stderr)
                return
            }
            self.session = session
            VTSessionSetProperty(session, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)
            fputs("nativepipe-remote: H264 VideoToolbox \(usingHardware ? "hardware" : "software") session ready\n", stderr)
            fflush(stderr)
        } else {
            fputs("nativepipe-remote: VT session create status=\(status)\n", stderr)
            fflush(stderr)
        }
    }

    /// One access unit: all VCL NALs length-prefixed (AVCC) in a single sample.
    private func decodeAccessUnit(_ nals: [Data], resourceID: UInt32) -> Bool {
        guard let session, let formatDescription else { return false }
        var packet = Data()
        for nal in nals {
            var length = UInt32(nal.count).bigEndian
            packet.append(Data(bytes: &length, count: 4))
            packet.append(nal)
        }
        let packetCount = packet.count

        var blockBuffer: CMBlockBuffer?
        let createStatus = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: nil,
            blockLength: packetCount,
            blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: packetCount,
            flags: 0,
            blockBufferOut: &blockBuffer)
        guard createStatus == noErr, let blockBuffer else { return false }
        let copyStatus = packet.withUnsafeBytes { raw -> OSStatus in
            guard let base = raw.baseAddress else { return -1 }
            return CMBlockBufferReplaceDataBytes(
                with: base, blockBuffer: blockBuffer, offsetIntoDestination: 0,
                dataLength: packetCount)
        }
        guard copyStatus == noErr else { return false }

        var sampleSize = packetCount
        var sampleBuffer: CMSampleBuffer?
        var timing = CMSampleTimingInfo(
            duration: .invalid,
            presentationTimeStamp: CMClockGetTime(CMClockGetHostTimeClock()),
            decodeTimeStamp: .invalid)
        let sampleStatus = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: blockBuffer,
            formatDescription: formatDescription,
            sampleCount: 1,
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timing,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &sampleSize,
            sampleBufferOut: &sampleBuffer)
        guard sampleStatus == noErr, let sampleBuffer else { return false }

        let result = DecodeResult()
        let status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: sampleBuffer,
            flags: [], infoFlagsOut: nil) { status, _, pixel, _, _ in
                result.status = status; result.pixel = pixel
            }
        guard status == noErr, result.status == noErr, let pixel = result.pixel,
              CVPixelBufferGetWidth(pixel) == width, CVPixelBufferGetHeight(pixel) == height else {
            return false
        }
        onFrame?(resourceID, pixel)
        return true
    }

    private static func splitAnnexB(_ data: Data) -> [Data] {
        var nals: [Data] = []
        var i = data.startIndex
        while i < data.endIndex {
            guard let start = findStartCode(data, from: i) else { break }
            let nalStart = start.nalStart
            let next = findStartCode(data, from: nalStart).map(\.codeStart) ?? data.endIndex
            if nalStart < next {
                nals.append(data.subdata(in: nalStart..<next))
            }
            i = next
        }
        return nals
    }

    private static func findStartCode(_ data: Data, from: Data.Index) -> (codeStart: Data.Index, nalStart: Data.Index)? {
        var i = from
        while i + 3 < data.endIndex {
            if data[i] == 0, data[i + 1] == 0 {
                if data[i + 2] == 1 {
                    return (i, i + 3)
                }
                if i + 4 <= data.endIndex, data[i + 2] == 0, data[i + 3] == 1 {
                    return (i, i + 4)
                }
            }
            i += 1
        }
        return nil
    }
}
