import CoreVideo
import XCTest
import NativePipeProtocol
@testable import NativePipeRemote

final class AV1DecoderTests: XCTestCase {
    static func packets(from fixture: URL? = nil) throws -> [Data] {
        let url = try XCTUnwrap(fixture ?? Bundle.module.url(forResource: "lifetime", withExtension: "av1", subdirectory: "Resources"))
        let bytes = [UInt8](try Data(contentsOf: url))
        var position = 0, packets: [Data] = []
        while position < bytes.count {
            guard position + 4 <= bytes.count else { throw RemoteError.message("Bad fixture length") }
            let length = (0..<4).reduce(0) { $0 | Int(bytes[position + $1]) << ($1 * 8) }
            position += 4
            guard length > 0, length <= bytes.count - position else { throw RemoteError.message("Bad fixture payload") }
            packets.append(Data(bytes[position..<position + length])); position += length
        }
        return packets
    }

    func testOptInDecodeBenchmark() throws {
        let env = ProcessInfo.processInfo.environment
        guard let path = env["NATIVEPIPE_BENCH_AV1_PACKETS"],
              let width = Int(env["NATIVEPIPE_BENCH_WIDTH"] ?? ""),
              let height = Int(env["NATIVEPIPE_BENCH_HEIGHT"] ?? "") else {
            throw XCTSkip("Opt-in production AV1 decoder component measurement")
        }
        let packets = try Self.packets(from: URL(fileURLWithPath: path))
        XCTAssertGreaterThan(packets.count, 10)
        for automatic in [false, true] {
            let decoder = AV1Decoder(allowHardware: automatic)
            var count = 0, failures = 0, hardware = 0, samples: [Double] = []
            decoder.onFrame = { _, pixel in
                XCTAssertEqual(CVPixelBufferGetWidth(pixel), width)
                XCTAssertEqual(CVPixelBufferGetHeight(pixel), height)
                count += 1
            }
            decoder.onFailure = { _ in failures += 1 }
            for (index, packet) in packets.enumerated() {
                let start = ProcessInfo.processInfo.systemUptime
                autoreleasepool {
                    decoder.decode(obu: packet, width: width, height: height,
                        bitstreamEpoch: 1, resourceID: UInt32(index + 1))
                }
                if index >= 10 { samples.append((ProcessInfo.processInfo.systemUptime - start) * 1000) }
                if decoder.usingHardware { hardware += 1 }
            }
            XCTAssertEqual(count, packets.count); XCTAssertEqual(failures, 0)
            samples.sort()
            guard !samples.isEmpty else { continue }
            print("AV1_DECODE_BENCH automatic=\(automatic) hardware_frames=\(hardware) width=\(width) height=\(height) frames=\(count) mean_ms=\(samples.reduce(0, +) / Double(samples.count)) p50_ms=\(samples[samples.count / 2]) p95_ms=\(samples[(samples.count * 95 - 1) / 100]) max_ms=\(samples.last!)")
        }
    }

    func testSoftwareInterFramesColorsOwnershipAndEpochReset() throws {
        let packets = try Self.packets()
        XCTAssertEqual(packets.count, 8)
        let decoder = AV1Decoder(allowHardware: false)
        var frames: [CVPixelBuffer] = [], ids: [UInt32] = [], failures: [UInt32] = []
        decoder.onFrame = { ids.append($0); frames.append($1) }
        decoder.onFailure = { failures.append($0) }
        for (index, packet) in packets.enumerated() {
            decoder.decode(obu: packet, width: 128, height: 96, bitstreamEpoch: 1, resourceID: UInt32(index + 1))
        }
        XCTAssertEqual(ids, Array(1...8)); XCTAssertTrue(failures.isEmpty)
        // Inspect retained old frames after all later frames have been decoded.
        for (index, frame) in frames.enumerated() {
            CVPixelBufferLockBaseAddress(frame, .readOnly)
            let p = CVPixelBufferGetBaseAddress(frame)!.advanced(by: 30 * CVPixelBufferGetBytesPerRow(frame) + 20 * 4).assumingMemoryBound(to: UInt8.self)
            XCTAssertEqual(Int(p[0]), 240, accuracy: 18)
            XCTAssertEqual(Int(p[1]), 60, accuracy: 18)
            XCTAssertEqual(Int(p[2]), index * 30, accuracy: 18)
            XCTAssertEqual(p[3], 255)
            CVPixelBufferUnlockBaseAddress(frame, .readOnly)
        }
        decoder.decode(obu: packets[0], width: 128, height: 96, bitstreamEpoch: 2, resourceID: 9)
        XCTAssertEqual(ids.last, 9); XCTAssertFalse(decoder.usingHardware)
        decoder.decode(obu: packets[0], width: 130, height: 96, bitstreamEpoch: 3, resourceID: 10)
        XCTAssertEqual(failures, [10])
    }

    func testHardwareOrSoftwareProducesOneFramePerResource() throws {
        let decoder = AV1Decoder()
        var ids: [UInt32] = [], failures = 0
        decoder.onFrame = { id, _ in ids.append(id) }; decoder.onFailure = { _ in failures += 1 }
        for (index, packet) in try Self.packets().enumerated() {
            decoder.decode(obu: packet, width: 128, height: 96, bitstreamEpoch: 1, resourceID: UInt32(index + 1))
        }
        XCTAssertEqual(ids, Array(1...8)); XCTAssertEqual(failures, 0)
        print("AV1 automatic decoder: \(decoder.usingHardware ? "VideoToolbox hardware" : "dav1d software")")
    }

    func testAV1MediaHeaderUsesItsOwnCodecIdentifier() throws {
        let packet = try XCTUnwrap(Self.packets().first)
        let header = MediaWire.Header(codec: .av1, surfaceID: 4, resourceID: 9,
            width: 128, height: 96, ptsNanos: 7, payloadLength: UInt32(packet.count), bitstreamEpoch: 2)
        let parsed = try XCTUnwrap(MediaWire.Header.parse(from: header.encoded()))
        XCTAssertEqual(parsed.codec, .av1); XCTAssertEqual(parsed.codec.rawValue, 3)
        XCTAssertEqual(parsed.resourceID, 9); XCTAssertEqual(parsed.bitstreamEpoch, 2)
    }

    func testMalformedOBUsAreRejectedWithoutLargeAllocation() {
        for bytes: [UInt8] in [[0x80], [0x08], [0x0a, 0x80], [0x0a, 255,255,255,255,255,255,255,255], [0x0e, 1, 0], [0x0a, 8, 0], [0x32, 0, 0x32, 0]] {
            XCTAssertThrowsError(try AV1Decoder.sequenceHeader(in: Data(bytes)))
        }
    }
}
