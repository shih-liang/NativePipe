import XCTest
@testable import NativePipeProtocol

final class NotificationWireTests: XCTestCase {
    private func append<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var value = value.littleEndian
        Swift.withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
    }
    private func append(_ string: String, to data: inout Data) {
        append(UInt32(string.utf8.count), to: &data)
        data.append(contentsOf: string.utf8)
    }
    private func guestMessage(opcode: UInt8) -> Data {
        var data = Data(WindowWire.lifecycleMagic)
        data.append(contentsOf: [1, opcode, 0, 0])
        return data
    }

    private func assertRejected(_ data: Data, _ message: String = "", file: StaticString = #filePath, line: UInt = #line) {
        do {
            guard case .notificationRejected = try WindowWire.guestEvent(from: data) else {
                return XCTFail("Malformed optional notification was not rejected: \(message)", file: file, line: line)
            }
        } catch { XCTFail("Optional notification broke the channel: \(error)", file: file, line: line) }
    }

    func testUntrustedRevisionAndEnvelopeAreHandledSeparately() throws {
        var truncated = guestMessage(opcode: 38)
        append(UInt32(7), to: &truncated)
        truncated.append(contentsOf: [1, 2, 3])
        guard case .notificationRejected(id: 7, revision: nil) = try WindowWire.guestEvent(from: truncated) else {
            return XCTFail("A truncated revision must not close another revision")
        }
        var badEnvelope = guestMessage(opcode: 38)
        badEnvelope[4] = 2
        XCTAssertThrowsError(try WindowWire.guestEvent(from: badEnvelope))
        badEnvelope[4] = 1; badEnvelope[6] = 1
        XCTAssertThrowsError(try WindowWire.guestEvent(from: badEnvelope))
        XCTAssertThrowsError(try WindowWire.guestEvent(from: guestMessage(opcode: 2)), "Malformed structural events remain fatal")
    }

    func testBacklogResetHasAnExactPayloadAndCannotBreakTheChannel() throws {
        guard case .notificationBacklogReset = try WindowWire.guestEvent(from: guestMessage(opcode: 42)) else {
            return XCTFail("Missing optional backlog reset")
        }
        var malformed = guestMessage(opcode: 42); malformed.append(0)
        guard case .notificationRejected(id: nil, revision: nil) = try WindowWire.guestEvent(from: malformed) else {
            return XCTFail("A malformed reset must neither reset nor disconnect")
        }
    }

    func testNotificationPostedDecodesEveryField() throws {
        var data = guestMessage(opcode: 38)
        append(UInt32(7), to: &data); append(UInt64(1), to: &data)
        data.append(2)                 // critical
        append(Int32(-1), to: &data)   // server default timeout
        for text in ["Mail", "org.example.mail", "New message", "Hello 你好"] { append(text, to: &data) }
        append(UInt32(2), to: &data)
        for text in ["default", "Open", "archive", "Archive"] { append(text, to: &data) }

        guard case .notificationPosted(let notification) = try WindowWire.guestEvent(from: data) else {
            return XCTFail("not a notification")
        }
        XCTAssertEqual(notification.id, 7)
        XCTAssertEqual(notification.urgency, .critical)
        XCTAssertEqual(notification.timeoutMilliseconds, -1)
        XCTAssertEqual(notification.appName, "Mail")
        XCTAssertEqual(notification.desktopEntry, "org.example.mail")
        XCTAssertEqual(notification.summary, "New message")
        XCTAssertEqual(notification.body, "Hello 你好")
        XCTAssertEqual(notification.actions, [.init(key: "default", label: "Open"),
                                              .init(key: "archive", label: "Archive")])
    }

    func testMalformedNotificationsAreRejected() throws {
        func posted(id: UInt32 = 1, urgency: UInt8 = 1, actions: UInt32 = 0, trailing: Bool = false) -> Data {
            var data = guestMessage(opcode: 38)
            append(id, to: &data); append(UInt64(1), to: &data); data.append(urgency); append(Int32(0), to: &data)
            for _ in 0..<4 { append("x", to: &data) }
            append(actions, to: &data)
            if trailing { data.append(0) }
            return data
        }
        XCTAssertNoThrow(try WindowWire.guestEvent(from: posted()))
        assertRejected(posted(id: 0), "id 0 is reserved")
        assertRejected(posted(urgency: 3))
        assertRejected(posted(actions: 9), "more than 8 actions")
        assertRejected(posted(trailing: true))

        var truncated = posted()
        truncated.removeLast(6)
        assertRejected(truncated)
    }

    func testNotificationFieldsUseTheGuestByteBudgets() throws {
        func posted(_ fields: [String], actions: [String] = [], timeout: Int32 = -1) -> Data {
            var data = guestMessage(opcode: 38)
            append(UInt32(1), to: &data); append(UInt64(1), to: &data); data.append(1); append(timeout, to: &data)
            for text in fields { append(text, to: &data) }
            append(UInt32(actions.count / 2), to: &data)
            for text in actions { append(text, to: &data) }
            return data
        }
        let budgets = [WindowWire.maximumNotificationNameSize, WindowWire.maximumNotificationNameSize,
                       WindowWire.maximumNotificationSummarySize, WindowWire.maximumNotificationBodySize]
        let fields = budgets.map { String(repeating: "é", count: $0 / 2) }
        let action = String(repeating: "é", count: WindowWire.maximumNotificationActionSize / 2)
        XCTAssertNoThrow(try WindowWire.guestEvent(from: posted(fields, actions: [action, action])))
        for index in fields.indices {
            var oversized = fields
            oversized[index] += "x"
            assertRejected(posted(oversized), "field \(index)")
        }
        assertRejected(posted(fields, actions: [action + "x", action]))
        assertRejected(posted(fields, actions: [action, action + "x"]))
        assertRejected(posted(fields, timeout: -2))
    }

    /// Bytes produced by the guest compositor's own C encoder
    /// (`np_window_put_*` in guest/compositor/windowwire.c, built exactly as
    /// notifications.c builds the message). Pinning them keeps the two
    /// implementations of the format from drifting apart.
    func testBytesFromTheGuestCEncoderDecode() throws {
        func data(hex: String) -> Data {
            var bytes = Data()
            var index = hex.startIndex
            while index < hex.endIndex {
                let next = hex.index(index, offsetBy: 2)
                bytes.append(UInt8(hex[index..<next], radix: 16)!)
                index = next
            }
            return bytes
        }
        let posted = data(hex: "4e505732012600000700000001000000000000000288130000040000004d61696c100000006f72672e6578616d706c652e6d61696c0b0000004e6577206d6573736167650a00000048656c6c6f20f09f918b020000000700000064656661756c74040000004f70656e050000007265706c79050000005265706c79")
        guard case .notificationPosted(let notification) = try WindowWire.guestEvent(from: posted) else {
            return XCTFail("not a notification")
        }
        XCTAssertEqual(notification.id, 7)
        XCTAssertEqual(notification.urgency, .critical)
        XCTAssertEqual(notification.timeoutMilliseconds, 5000)
        XCTAssertEqual(notification.appName, "Mail")
        XCTAssertEqual(notification.desktopEntry, "org.example.mail")
        XCTAssertEqual(notification.summary, "New message")
        XCTAssertEqual(notification.body, "Hello \u{1F44B}")
        XCTAssertEqual(notification.actions.map(\.key), ["default", "reply"])
        XCTAssertEqual(notification.actions.map(\.label), ["Open", "Reply"])

        guard case .notificationClosed(let id, let revision) = try WindowWire.guestEvent(from: data(hex: "4e505732012700000c0000007b00000000000000")) else {
            return XCTFail("not a close")
        }
        XCTAssertEqual(id, 12); XCTAssertEqual(revision, 123)
    }

    func testNotificationClosedDecodes() throws {
        var data = guestMessage(opcode: 39)
        append(UInt32(12), to: &data); append(UInt64(123), to: &data)
        guard case .notificationClosed(let id, let revision) = try WindowWire.guestEvent(from: data) else {
            return XCTFail("not a close")
        }
        XCTAssertEqual(id, 12); XCTAssertEqual(revision, 123)
        var zero = guestMessage(opcode: 39)
        append(UInt32(0), to: &zero)
        assertRejected(zero)
    }

    func testHostCommandsEncodeWithTheirOpcodes() throws {
        let closed = try WindowWire.commandPayload(for: .notificationClosed(id: 5, revision: 9, reason: .dismissed))
        var expectedClosed = Data(WindowWire.lifecycleMagic)
        expectedClosed.append(contentsOf: [2, 29, 0, 0])
        append(UInt32(5), to: &expectedClosed); append(UInt64(9), to: &expectedClosed); append(UInt32(2), to: &expectedClosed)
        XCTAssertEqual(closed, expectedClosed)

        let action = try WindowWire.commandPayload(for: .notificationAction(id: 5, revision: 9, key: "default"))
        var expectedAction = Data(WindowWire.lifecycleMagic)
        expectedAction.append(contentsOf: [2, 30, 0, 0])
        append(UInt32(5), to: &expectedAction); append(UInt64(9), to: &expectedAction); append("default", to: &expectedAction)
        XCTAssertEqual(action, expectedAction)

        XCTAssertThrowsError(try WindowWire.commandPayload(for: .notificationClosed(id: 0, revision: 1, reason: .expired)))
        XCTAssertThrowsError(try WindowWire.commandPayload(
            for: .notificationAction(id: 1, revision: 1, key: String(repeating: "k", count: 129))))
        XCTAssertNoThrow(try WindowWire.commandPayload(
            for: .notificationAction(id: 1, revision: 1, key: String(repeating: "é", count: 64))))
    }

    func testNotificationCommandsUseTheControlLane() {
        XCTAssertEqual(Windowing.HostCommand.notificationAction(id: 1, revision: 1, key: "default").transportLane, .control)
        XCTAssertEqual(Windowing.HostCommand.notificationClosed(id: 1, revision: 1, reason: .expired).transportLane, .control)
    }
}
