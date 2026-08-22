import Foundation
import NativePipeProtocol
import XCTest

final class MediaWireSmokeTests: XCTestCase {
    private func append<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var value = value.littleEndian
        Swift.withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
    }

    private func append(_ value: Float, to data: inout Data) {
        append(value.bitPattern, to: &data)
    }

    func testBinarySceneSnapshotDecodesWithExactBounds() throws {
        var payload = Data(WindowWire.sceneMagic)
        append(WindowWire.sceneVersion, to: &payload)
        append(UInt16(WindowWire.sceneHeaderSize), to: &payload)
        append(UInt32(WindowWire.sceneHeaderSize + WindowWire.sceneLayerSize), to: &payload)
        append(UInt32(7), to: &payload)   // root surface
        append(UInt32(19), to: &payload)  // presentation
        append(UInt32(800), to: &payload)
        append(UInt32(600), to: &payload)
        append(UInt32(2), to: &payload)
        append(Int32(-8), to: &payload)
        append(Int32(-4), to: &payload)
        append(Int32(400), to: &payload)
        append(Int32(300), to: &payload)
        append(UInt32(1), to: &payload)
        append(UInt32(0), to: &payload)

        append(UInt32(9), to: &payload)
        append(UInt32(123), to: &payload)
        append(UInt32(800), to: &payload)
        append(UInt32(600), to: &payload)
        append(UInt32(3200), to: &payload)
        append(UInt16(1), to: &payload)
        append(UInt16(1), to: &payload) // opaque
        append(UInt32(0), to: &payload)
        append(UInt32(0), to: &payload)
        for value: Float in [0, 0, 800, 600] { append(value, to: &payload) }
        for value: Float in [0, 0, 800, 600] { append(value, to: &payload) }
        for value: Float in [0, 0, 800, 600] { append(value, to: &payload) }
        append(Float(1), to: &payload)
        append(UInt32(0), to: &payload)

        guard case .sceneCommitted(let scene) = try WindowWire.guestEvent(from: payload)
        else { return XCTFail("not a scene") }
        XCTAssertEqual(scene.surface, 7)
        XCTAssertEqual(scene.presentationID, 19)
        XCTAssertEqual(scene.windowGeometry, .init(x: -8, y: -4, width: 400, height: 300))
        XCTAssertEqual(scene.layers.count, 1)
        XCTAssertEqual(scene.layers[0].resourceID, 123)
        XCTAssertTrue(scene.layers[0].opaque)
    }

    func testBinarySceneSnapshotRejectsCountBeyondPayload() {
        var payload = Data(repeating: 0, count: WindowWire.sceneHeaderSize)
        payload.replaceSubrange(0..<4, with: WindowWire.sceneMagic)
        func write<T: FixedWidthInteger>(_ value: T, at offset: Int) {
            var little = value.littleEndian
            Swift.withUnsafeBytes(of: &little) {
                payload.replaceSubrange(offset..<(offset + $0.count), with: $0)
            }
        }
        write(WindowWire.sceneVersion, at: 4)
        write(UInt16(WindowWire.sceneHeaderSize), at: 6)
        write(UInt32(payload.count), at: 8)
        write(UInt32(1), at: 12)
        write(UInt32(1), at: 16)
        write(UInt32(1), at: 20)
        write(UInt32(1), at: 24)
        write(UInt32(1), at: 28)
        write(Int32(1), at: 40)
        write(Int32(1), at: 44)
        write(UInt32(128), at: 48)
        XCTAssertThrowsError(try WindowWire.guestEvent(from: payload))
    }

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
