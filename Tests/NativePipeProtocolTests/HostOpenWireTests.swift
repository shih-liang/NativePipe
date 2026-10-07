import XCTest
@testable import NativePipeProtocol

final class HostOpenWireTests: XCTestCase {
    private func data(hex: String) -> Data {
        var bytes = Data()
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            bytes.append(UInt8(hex[index..<next], radix: 16)!)
            index = next
        }
        return bytes
    }

    /// Frames produced by np-open's C encoder (guest/session/test-np-open.c).
    func testFramesFromTheGuestToolDecode() throws {
        let url = try HostOpenWire.decodeRequest(from: data(hex:
            "4e504f50010100001900000068747470733a2f2f6578616d706c652e636f6d2f613f623d31"))
        XCTAssertEqual(url, .init(kind: .url, value: "https://example.com/a?b=1"))
        let file = try HostOpenWire.decodeRequest(from: data(hex:
            "4e504f5001020000140000002f686f6d652f6465762fe69687e6a1a32e706466"))
        XCTAssertEqual(file, .init(kind: .file, value: "/home/dev/文档.pdf"))
    }

    func testEncodingMatchesTheGuestToolByteForByte() throws {
        XCTAssertEqual(try HostOpenWire.encode(.init(kind: .url, value: "https://example.com/a?b=1")),
                       data(hex: "4e504f50010100001900000068747470733a2f2f6578616d706c652e636f6d2f613f623d31"))
    }

    func testResponsesRoundTrip() throws {
        for response in [HostOpenWire.Response(status: .opened),
                         .init(status: .refused, message: "That kind of link is not allowed."),
                         .init(status: .disabled, message: "文字"),
                         .init(status: .failed, message: "x")] {
            XCTAssertEqual(try HostOpenWire.decodeResponse(from: HostOpenWire.encode(response)), response)
        }
        let long = HostOpenWire.Response(status: .failed, message: String(repeating: "é", count: 600))
        let framed = HostOpenWire.encode(long)
        XCTAssertLessThanOrEqual(framed.count - HostOpenWire.headerSize, HostOpenWire.maximumMessage)
        XCTAssertNoThrow(try HostOpenWire.decodeResponse(from: framed), "Truncation keeps the text valid UTF-8.")
    }

    func testMalformedRequestsAreRejected() throws {
        let valid = try HostOpenWire.encode(.init(kind: .url, value: "https://example.com"))
        func mutated(_ index: Int, _ value: UInt8) -> Data { var copy = valid; copy[index] = value; return copy }
        XCTAssertNoThrow(try HostOpenWire.decodeRequest(from: valid))
        XCTAssertThrowsError(try HostOpenWire.decodeRequest(from: mutated(0, 0x58)), "magic")
        XCTAssertThrowsError(try HostOpenWire.decodeRequest(from: mutated(4, 2)), "version")
        XCTAssertThrowsError(try HostOpenWire.decodeRequest(from: mutated(5, 9)), "kind")
        XCTAssertThrowsError(try HostOpenWire.decodeRequest(from: mutated(6, 1)), "reserved")
        XCTAssertThrowsError(try HostOpenWire.decodeRequest(from: valid.dropLast()), "truncated payload")
        XCTAssertThrowsError(try HostOpenWire.decodeRequest(from: valid + Data([0x41])), "trailing bytes")
        XCTAssertThrowsError(try HostOpenWire.decodeRequest(from: valid.prefix(5)), "short header")

        var empty = valid.prefix(HostOpenWire.headerSize)
        empty.replaceSubrange(8..<12, with: [0, 0, 0, 0])
        XCTAssertThrowsError(try HostOpenWire.decodeRequest(from: Data(empty)), "empty payload")
        var oversize = valid.prefix(HostOpenWire.headerSize)
        oversize.replaceSubrange(8..<12, with: [0x01, 0x20, 0, 0]) // 8193
        XCTAssertThrowsError(try HostOpenWire.payloadLength(inRequestHeader: Data(oversize)), "over the limit")
    }

    func testControlCharactersAndInvalidUTF8AreRejected() throws {
        for bad in ["https://a/\nb", "https://a/\u{7}", "a\u{0}b", "https://a/\u{7f}"] {
            var frame = Data("NPOP".utf8) + Data([1, 1, 0, 0])
            var length = UInt32(bad.utf8.count).littleEndian
            withUnsafeBytes(of: &length) { frame.append(contentsOf: $0) }
            frame.append(Data(bad.utf8))
            XCTAssertThrowsError(try HostOpenWire.decodeRequest(from: frame), bad.debugDescription)
        }
        var invalid = Data("NPOP".utf8) + Data([1, 1, 0, 0, 2, 0, 0, 0, 0xff, 0xfe])
        XCTAssertThrowsError(try HostOpenWire.decodeRequest(from: invalid))
        invalid = Data()
        XCTAssertThrowsError(try HostOpenWire.decodeRequest(from: invalid))
    }

    func testEncodeRefusesEmptyAndOversizeValues() {
        XCTAssertThrowsError(try HostOpenWire.encode(.init(kind: .url, value: "")))
        XCTAssertThrowsError(try HostOpenWire.encode(.init(kind: .file, value: String(repeating: "a", count: 8193))))
    }

    func testThePortIsStable() {
        XCTAssertEqual(NativePipePort.hostOpen, 1030, "np.h defines NP_PORT_HOST_OPEN with the same value.")
    }
}
