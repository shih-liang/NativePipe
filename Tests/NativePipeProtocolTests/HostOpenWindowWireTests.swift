import XCTest
@testable import NativePipeProtocol

final class HostOpenWindowWireTests: XCTestCase {
    private func append(_ value: UInt32, to data: inout Data) {
        var value = value.littleEndian
        withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
    }
    private func event(_ opcode: UInt8, token: UInt32, frame: Data? = nil) -> Data {
        var data = Data("NPW2".utf8)
        data.append(contentsOf: [1, opcode, 0, 0])
        append(token, to: &data)
        if let frame { append(UInt32(frame.count), to: &data); data.append(frame) }
        return data
    }

    func testRequestKeepsTheSameHostOpenFrame() throws {
        let request = HostOpenWire.Request(kind: .url, value: "https://example.com")
        let frame = try HostOpenWire.encode(request)
        let payload = event(40, token: 1, frame: frame)
        guard case .hostOpenRequested(let token, let decoded) = try WindowWire.guestEvent(from: payload) else {
            return XCTFail("not a host open request")
        }
        XCTAssertEqual(token, 1)
        XCTAssertEqual(decoded, request)
        // The C socket test forwards this exact NPOP frame and token through
        // np_window_put_bytes; no alternate SSH-only open protocol exists.
        XCTAssertEqual(frame.count, 31)
        XCTAssertEqual(Array(payload.prefix(16)), [78, 80, 87, 50, 1, 40, 0, 0, 1, 0, 0, 0, 31, 0, 0, 0])
    }

    func testCancellationRequiresAnExactNonzeroToken() throws {
        guard case .hostOpenCancelled(let token) = try WindowWire.guestEvent(from: event(41, token: 9)) else {
            return XCTFail("not a cancellation")
        }
        XCTAssertEqual(token, 9)
        try assertRejected(event(41, token: 0), token: nil)
        try assertRejected(event(41, token: 9) + Data([0]), token: 9)
    }

    func testMalformedNestedRequestsAreRejected() throws {
        let valid = try HostOpenWire.encode(.init(kind: .file, value: "/tmp/report.pdf"))
        try assertRejected(event(40, token: 0, frame: valid), token: nil)
        try assertRejected(event(40, token: 1, frame: valid.dropLast()), token: 1)
        try assertRejected(event(40, token: 1, frame: valid) + Data([0]), token: 1)
        var malformed = valid
        malformed[5] = 9
        try assertRejected(event(40, token: 1, frame: malformed), token: 1)
        malformed = valid
        malformed[HostOpenWire.headerSize] = 10
        try assertRejected(event(40, token: 1, frame: malformed), token: 1)
        malformed = valid
        malformed[HostOpenWire.headerSize] = 0xff
        try assertRejected(event(40, token: 1, frame: malformed), token: 1)
        try assertRejected(event(40, token: 1,
            frame: Data(repeating: 0, count: HostOpenWire.headerSize + HostOpenWire.maximumPayload + 1)), token: 1)
    }

    private func assertRejected(_ payload: Data, token: UInt32?, file: StaticString = #filePath, line: UInt = #line) throws {
        guard case .hostOpenRejected(let decoded) = try WindowWire.guestEvent(from: payload) else {
            return XCTFail("Malformed optional content must not disconnect the display", file: file, line: line)
        }
        XCTAssertEqual(decoded, token, file: file, line: line)
    }

    func testTruncatedTokenIsNotBorrowedAndInvalidOuterHeaderStillFails() throws {
        let request = event(40, token: 9)
        try assertRejected(Data(request.prefix(10)), token: nil)
        var malformed = request
        malformed[4] = 0
        XCTAssertThrowsError(try WindowWire.guestEvent(from: malformed))
    }

    func testResponseUsesTheControlLaneAndBoundsItsFrame() throws {
        let response = HostOpenWire.Response(status: .opened, message: "Saved on the Mac.")
        let command = Windowing.HostCommand.hostOpenResponse(token: 7, response: response)
        XCTAssertEqual(command.transportLane, .control)
        let payload = try WindowWire.commandPayload(for: command)
        XCTAssertEqual(Array(payload.prefix(12)), [78, 80, 87, 50, 2, 31, 0, 0, 7, 0, 0, 0])
        XCTAssertEqual(try HostOpenWire.decodeResponse(from: Data(payload.dropFirst(16))), response)
        XCTAssertThrowsError(try WindowWire.commandPayload(for: .hostOpenResponse(token: 0, response: response)))
        let long = try WindowWire.commandPayload(for: .hostOpenResponse(token: 8,
            response: .init(status: .failed, message: String(repeating: "é", count: 700))))
        XCTAssertLessThanOrEqual(long.count, 16 + HostOpenWire.headerSize + HostOpenWire.maximumMessage)
        XCTAssertNoThrow(try HostOpenWire.decodeResponse(from: Data(long.dropFirst(16))))
    }

    func testThePreviousWindowProtocolIsRejected() {
        var ready = event(1, token: 123)
        append(UInt32(14), to: &ready)
        XCTAssertEqual(WindowWire.windowProtocolVersion, 15)
        XCTAssertThrowsError(try WindowWire.guestEvent(from: ready)) {
            XCTAssertEqual($0 as? WindowWire.DecodeError, .unsupportedWindowVersion(14))
        }
    }
}
