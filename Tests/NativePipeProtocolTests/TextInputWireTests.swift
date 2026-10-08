import XCTest
@testable import NativePipeProtocol

final class TextInputWireTests: XCTestCase {
    private func append(_ value: UInt32, to data: inout Data) {
        var value = value.littleEndian
        withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
    }
    private func event(_ opcode: UInt8, fields: [UInt32]) -> Data {
        var data = Data("NPW2".utf8)
        data.append(contentsOf: [1, opcode, 0, 0])
        for field in fields { append(field, to: &data) }
        return data
    }
    private func surrounding(_ text: String, cursor: Int32, anchor: Int32) -> Data {
        var data = event(33, fields: [7, UInt32(text.utf8.count)])
        data.append(contentsOf: text.utf8)
        append(UInt32(bitPattern: cursor), to: &data)
        append(UInt32(bitPattern: anchor), to: &data)
        return data
    }

    func testContentTypePreservesHintsPurposeAndCause() throws {
        guard case .textInputContentType(let window, let hints, let purpose, let cause) =
            try WindowWire.guestEvent(from: event(46, fields: [7, 0x180, 8, 1])) else {
            return XCTFail("missing content type")
        }
        XCTAssertEqual(window, 7); XCTAssertEqual(hints, 0x180)
        XCTAssertEqual(purpose, 8); XCTAssertEqual(cause, 1)
        for fields: [UInt32] in [[0, 0, 0, 0], [7, 0x400, 0, 0], [7, 0, 14, 0], [7, 0, 0, 2]] {
            XCTAssertThrowsError(try WindowWire.guestEvent(from: event(46, fields: fields)))
        }
        XCTAssertThrowsError(try WindowWire.guestEvent(from: event(46, fields: [7, 0, 0, 0]) + Data([0])))
    }

    func testEnabledStateCarriesAnIndependentFieldEpoch() throws {
        let payload = event(31, fields: [7, 11]) + Data([1])
        guard case .textInputEnabled(let window, let epoch, let enabled) = try WindowWire.guestEvent(from: payload) else {
            return XCTFail("missing enabled state")
        }
        XCTAssertEqual(window, 7); XCTAssertEqual(epoch, 11); XCTAssertTrue(enabled)
        XCTAssertThrowsError(try WindowWire.guestEvent(from: event(31, fields: [7, 0]) + Data([1])))
        XCTAssertThrowsError(try WindowWire.commandPayload(for: .textCommit(window: 7, epoch: 0, text: "中")))
        XCTAssertThrowsError(try WindowWire.commandPayload(for: .textPreedit(window: 7, epoch: 0, text: "", cursorBegin: 0, cursorEnd: 0)))
    }

    func testSurroundingOffsetsMustEndOnUTF8Scalars() throws {
        guard case .textInputSurroundingText(_, let text, let cursor, let anchor) =
            try WindowWire.guestEvent(from: surrounding("中😀", cursor: 3, anchor: 7)) else {
            return XCTFail("missing surrounding text")
        }
        XCTAssertEqual(text, "中😀"); XCTAssertEqual(cursor, 3); XCTAssertEqual(anchor, 7)
        for position: Int32 in [-1, 1, 4, 8] {
            XCTAssertThrowsError(try WindowWire.guestEvent(from: surrounding("中😀", cursor: position, anchor: 7)))
        }
        XCTAssertThrowsError(try WindowWire.guestEvent(from: surrounding(String(repeating: "a", count: 4001), cursor: 0, anchor: 0)))
    }

    func testAtomicEditKeepsNilSeparateFromEmptyPreedit() throws {
        let command = Windowing.HostCommand.textEdit(window: 7, epoch: 11, commit: "文", preedit: "",
            cursorBegin: 0, cursorEnd: 0, beforeLength: 3, afterLength: 4)
        XCTAssertEqual(command.transportLane, .input)
        let payload = try WindowWire.commandPayload(for: command)
        var expected = Data("NPW2".utf8)
        expected.append(contentsOf: [2, 33, 0, 0])
        for field: UInt32 in [7, 11, 3, 4] { append(field, to: &expected) }
        expected.append(1); append(3, to: &expected); expected.append(contentsOf: "文".utf8)
        expected.append(1); append(0, to: &expected)
        append(0, to: &expected); append(0, to: &expected)
        XCTAssertEqual(payload, expected)
        let preedit = try WindowWire.commandPayload(for: .textEdit(window: 7, epoch: 11, commit: nil, preedit: "😀",
            cursorBegin: 4, cursorEnd: 4, beforeLength: 0, afterLength: 0))
        XCTAssertEqual(preedit[24], 0, "nil commit is absent, not an empty commit event")
        XCTAssertEqual(preedit[25], 1)
    }

    func testMalformedPreeditCannotSplitEmojiOrSendAnEmptyBatch() {
        for position in [1, 2, 3, 5] {
            XCTAssertThrowsError(try WindowWire.commandPayload(for: .textPreedit(window: 7, epoch: 11, text: "😀",
                cursorBegin: position, cursorEnd: position)))
            XCTAssertThrowsError(try WindowWire.commandPayload(for: .textEdit(window: 7, epoch: 11, commit: nil, preedit: "😀",
                cursorBegin: position, cursorEnd: position, beforeLength: 0, afterLength: 0)))
        }
        XCTAssertNoThrow(try WindowWire.commandPayload(for: .textPreedit(window: 7, epoch: 11, text: "😀", cursorBegin: -1, cursorEnd: -1)))
        XCTAssertThrowsError(try WindowWire.commandPayload(for: .textEdit(window: 7, epoch: 11, commit: nil, preedit: nil,
            cursorBegin: 0, cursorEnd: 0, beforeLength: 0, afterLength: 0)))
        XCTAssertThrowsError(try WindowWire.commandPayload(for: .textEdit(window: 7, epoch: 11, commit: nil, preedit: "",
            cursorBegin: -1, cursorEnd: 0, beforeLength: 0, afterLength: 0)))
    }
}
