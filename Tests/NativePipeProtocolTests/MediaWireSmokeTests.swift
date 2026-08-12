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
}
