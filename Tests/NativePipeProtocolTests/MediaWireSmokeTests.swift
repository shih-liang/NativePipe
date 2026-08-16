import Foundation
import NativePipeProtocol
import XCTest

final class MediaWireSmokeTests: XCTestCase {
    func testNPENRoundTrip() {
        let header = MediaWire.Header(
            surfaceID: 17, width: 320, height: 200,
            ptsNanos: 123, payloadLength: 4, bitstreamEpoch: 2)
        var frame = header.encoded()
        XCTAssertEqual(frame.count, MediaWire.headerSize)
        frame.append(contentsOf: [0, 0, 0, 1])
        let demux = MediaWire.Demuxer()
        let out = demux.push(frame)
        XCTAssertEqual(out.count, 1)
        XCTAssertEqual(out[0].0.surfaceID, 17)
        XCTAssertEqual(out[0].0.width, 320)
        XCTAssertEqual(out[0].0.bitstreamEpoch, 2)
        XCTAssertEqual(out[0].1, Data([0, 0, 0, 1]))
    }

    func testEncodedFrameSourceKind() throws {
        let frame = Windowing.Frame(
            resourceID: 9, width: 10, height: 10, bytesPerRow: 40,
            format: .bgra8888, source: .encoded, codec: "h264", bitstreamEpoch: 3)
        let data = try JSONEncoder().encode(frame)
        let decoded = try JSONDecoder().decode(Windowing.Frame.self, from: data)
        XCTAssertEqual(decoded.source, .encoded)
        XCTAssertEqual(decoded.codec, "h264")
        XCTAssertEqual(decoded.bitstreamEpoch, 3)
    }

    func testExecWireSurvivesFragmentationAndMagicInPTYData() throws {
        let bytes = Data("before-NPXT-after".utf8)
        let dataFrame = try ExecWire.data(bytes)
        let resizeFrame = try ExecWire.resize(cols: 132, rows: 44)
        let stream = dataFrame + resizeFrame
        var decoder = ExecWire.Decoder()
        for byte in stream {
            decoder.append(Data([byte]))
        }
        let first = try XCTUnwrap(decoder.next())
        XCTAssertEqual(first.kind, .data)
        XCTAssertEqual(first.payload, bytes)
        let second = try XCTUnwrap(decoder.next())
        XCTAssertEqual(second.kind, .resize)
        XCTAssertEqual(second.payload.count, 8)
        XCTAssertNil(try decoder.next())
    }

    func testMediaDemuxerRejectsOversizedFrameAndResynchronizes() {
        var invalid = MediaWire.Header(
            surfaceID: 1, width: 1, height: 1, ptsNanos: 0,
            payloadLength: UInt32(MediaWire.maximumPayloadSize + 1)).encoded()
        let validHeader = MediaWire.Header(
            surfaceID: 99, width: 2, height: 3, ptsNanos: 4,
            payloadLength: 1).encoded()
        invalid.append(validHeader)
        invalid.append(0x7f)
        let output = MediaWire.Demuxer().push(invalid)
        XCTAssertEqual(output.count, 1)
        XCTAssertEqual(output[0].0.surfaceID, 99)
        XCTAssertEqual(output[0].1, Data([0x7f]))
    }
}
