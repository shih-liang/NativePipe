import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

/// Incremental H.264 Annex-B → CVPixelBuffer decoder (VideoToolbox).
final class H264Decoder {
    private var session: VTDecompressionSession?
    private var formatDescription: CMVideoFormatDescription?
    private var sps: Data?
    private var pps: Data?
    private var width: Int = 0
    private var height: Int = 0
    private var epoch: UInt16 = 0

    var onFrame: ((CVPixelBuffer) -> Void)?

    func reset() {
        if let session {
            VTDecompressionSessionWaitForAsynchronousFrames(session)
            VTDecompressionSessionInvalidate(session)
        }
        session = nil
        formatDescription = nil
        sps = nil
        pps = nil
        epoch = 0
    }

    deinit { reset() }

    func decode(annexB: Data, width: Int, height: Int, bitstreamEpoch: UInt16) {
        if bitstreamEpoch != 0, bitstreamEpoch != epoch {
            reset()
            epoch = bitstreamEpoch
        }
        self.width = width
        self.height = height

        let nals = Self.splitAnnexB(annexB)
        guard !nals.isEmpty else {
            fputs("nativepipe-remote: H264: 0 NALs in \(annexB.count) bytes\n", stderr)
            fflush(stderr)
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
        if formatChanged { rebuildFormatIfPossible() }
        guard !vcl.isEmpty else { return }
        guard session != nil else {
            fputs("nativepipe-remote: H264: VCL without VT session\n", stderr)
            fflush(stderr)
            return
        }
        decodeAccessUnit(vcl)
    }

    private func rebuildFormatIfPossible() {
        guard let sps, let pps else { return }
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
            return
        }
        if let description {
            formatDescription = description
            recreateSession(description)
        }
    }

    private func recreateSession(_ description: CMVideoFormatDescription) {
        if let session {
            VTDecompressionSessionInvalidate(session)
            self.session = nil
        }
        var callback = VTDecompressionOutputCallbackRecord(
            decompressionOutputCallback: { refcon, _, status, _, imageBuffer, _, _ in
                guard let refcon else { return }
                let decoder = Unmanaged<H264Decoder>.fromOpaque(refcon).takeUnretainedValue()
                if status == noErr, let imageBuffer {
                    decoder.onFrame?(imageBuffer)
                } else if status != noErr {
                    fputs("nativepipe-remote: VT callback status=\(status)\n", stderr)
                }
            },
            decompressionOutputRefCon: Unmanaged.passUnretained(self).toOpaque()
        )
        var session: VTDecompressionSession?
        let attrs: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
        ]
        let status = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            formatDescription: description,
            decoderSpecification: nil,
            imageBufferAttributes: attrs as CFDictionary,
            outputCallback: &callback,
            decompressionSessionOut: &session)
        if status == noErr {
            self.session = session
            fputs("nativepipe-remote: H264 VT session ready\n", stderr)
            fflush(stderr)
        } else {
            fputs("nativepipe-remote: VT session create status=\(status)\n", stderr)
            fflush(stderr)
        }
    }

    /// One access unit: all VCL NALs length-prefixed (AVCC) in a single sample.
    private func decodeAccessUnit(_ nals: [Data]) {
        guard let session, let formatDescription else { return }
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
        guard createStatus == noErr, let blockBuffer else { return }
        let copyStatus = packet.withUnsafeBytes { raw -> OSStatus in
            guard let base = raw.baseAddress else { return -1 }
            return CMBlockBufferReplaceDataBytes(
                with: base, blockBuffer: blockBuffer, offsetIntoDestination: 0,
                dataLength: packetCount)
        }
        guard copyStatus == noErr else { return }

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
        guard sampleStatus == noErr, let sampleBuffer else { return }

        var flagsOut: VTDecodeInfoFlags = []
        let asynchronous = VTDecodeFrameFlags(rawValue: 1 << 0)
        let decodeStatus = VTDecompressionSessionDecodeFrame(
            session, sampleBuffer: sampleBuffer, flags: asynchronous,
            frameRefcon: nil, infoFlagsOut: &flagsOut)
        if decodeStatus != noErr {
            fputs("nativepipe-remote: VTDecode status=\(decodeStatus)\n", stderr)
            fflush(stderr)
        }
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
