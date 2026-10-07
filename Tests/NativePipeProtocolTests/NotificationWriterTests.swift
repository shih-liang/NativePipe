import Foundation
import XCTest
@testable import NativePipeProtocol

final class NotificationWriterTests: XCTestCase {
    private final class Sink: @unchecked Sendable {
        let condition = NSCondition()
        let entered = DispatchSemaphore(value: 0), resume = DispatchSemaphore(value: 0)
        var packets: [Data] = []
        var failures = 0
        var blockFirst = false
        func write(_ bytes: Data) {
            condition.lock(); let block = blockFirst; blockFirst = false; condition.unlock()
            if block { entered.signal(); _ = resume.wait(timeout: .now() + 5) }
            condition.lock(); packets.append(bytes); condition.broadcast(); condition.unlock()
        }
        func read(timeout: TimeInterval = 2) -> Data? {
            condition.lock(); defer { condition.unlock() }
            let deadline = Date().addingTimeInterval(timeout)
            while packets.isEmpty { if !condition.wait(until: deadline) { return nil } }
            return packets.removeFirst()
        }
        func failed() { condition.lock(); failures += 1; condition.unlock() }
    }
    private func writer(_ sink: Sink) throws -> WindowCommandWriter {
        let writer = WindowCommandWriter { _, bytes in sink.write(bytes) }
        writer.onFailure = { _ in sink.failed() }
        writer.install(try FileHandle(forWritingTo: URL(fileURLWithPath: "/dev/null")))
        return writer
    }
    private func post(_ writer: WindowCommandWriter, _ id: UInt32, _ revision: UInt64 = 1) {
        writer.observeGuestNotification(.notificationPosted(.init(id: id, revision: revision, summary: "Test")))
    }
    private func bytes(_ command: Windowing.HostCommand) throws -> Data {
        try WireFormat.frame(payload: WindowWire.commandPayload(for: command))
    }

    func testMoreThan64SequentialHostClosuresRecycleTracking() throws {
        let sink = Sink(), writer = try writer(sink); defer { writer.disconnect() }
        for id in UInt32(1)...250 {
            post(writer, id)
            let closed = Windowing.HostCommand.notificationClosed(id: id, revision: 1, reason: .undefined)
            writer.send(closed)
            XCTAssertEqual(try XCTUnwrap(sink.read()), try bytes(closed), "Host close must retire ID \(id) without a guest echo")
        }
        XCTAssertEqual(sink.failures, 0)
    }

    func testBlockedReplacementFloodKeepsLatestFeedbackAndWindowCommands() throws {
        let sink = Sink(); sink.blockFirst = true
        let writer = try writer(sink); defer { sink.resume.signal(); writer.disconnect() }
        let key = Windowing.HostCommand.key(window: 1, keycode: 30, pressed: true, modifiers: [])
        writer.send(key); XCTAssertEqual(sink.entered.wait(timeout: .now() + 2), .success)
        for revision in UInt64(1)...70_000 {
            post(writer, 1, revision)
            writer.send(.notificationClosed(id: 1, revision: revision, reason: .undefined))
        }
        // Unknown and malformed optional responses must not occupy any budget.
        for id in UInt32(100)...70_100 {
            writer.send(.notificationClosed(id: id, revision: 1, reason: .undefined))
            writer.send(.notificationAction(id: 1, revision: 70_000, key: "\0"))
        }
        let up = Windowing.HostCommand.key(window: 1, keycode: 30, pressed: false, modifiers: [])
        writer.send(up); sink.resume.signal()
        XCTAssertEqual(try XCTUnwrap(sink.read()), try bytes(key))
        XCTAssertEqual(try XCTUnwrap(sink.read()), try bytes(up))
        XCTAssertEqual(try XCTUnwrap(sink.read()), try bytes(.notificationClosed(id: 1, revision: 70_000, reason: .undefined)))
        XCTAssertNil(sink.read(timeout: 0.02)); XCTAssertEqual(sink.failures, 0)
        post(writer, 2); writer.send(.notificationClosed(id: 2, revision: 1, reason: .undefined))
        XCTAssertNotNil(sink.read())
    }

    func testActionPrecedesCloseAndGuestWithdrawalRetiresPendingPair() throws {
        let sink = Sink(); sink.blockFirst = true
        let writer = try writer(sink); defer { sink.resume.signal(); writer.disconnect() }
        let key = Windowing.HostCommand.key(window: 1, keycode: 30, pressed: true, modifiers: [])
        writer.send(key); XCTAssertEqual(sink.entered.wait(timeout: .now() + 2), .success)
        for id in UInt32(1)...64 {
            post(writer, id)
            writer.send(.notificationAction(id: id, revision: 1, key: "default"))
            writer.send(.notificationClosed(id: id, revision: 1, reason: .dismissed))
        }
        writer.observeGuestNotification(.notificationClosed(id: 1, revision: 1))
        post(writer, 2, 2)
        writer.send(.notificationAction(id: 2, revision: 1, key: "stale"))
        writer.send(.notificationClosed(id: 2, revision: 1, reason: .dismissed))
        writer.send(.notificationAction(id: 2, revision: 2, key: "current"))
        writer.send(.notificationClosed(id: 2, revision: 2, reason: .dismissed))
        post(writer, 65) // Withdrawal freed a slot before drain.
        writer.send(.notificationClosed(id: 65, revision: 1, reason: .undefined))
        sink.resume.signal(); XCTAssertEqual(try XCTUnwrap(sink.read()), try bytes(key))
        for id in UInt32(3)...64 {
            XCTAssertEqual(try XCTUnwrap(sink.read()), try bytes(.notificationAction(id: id, revision: 1, key: "default")))
            XCTAssertEqual(try XCTUnwrap(sink.read()), try bytes(.notificationClosed(id: id, revision: 1, reason: .dismissed)))
        }
        XCTAssertEqual(try XCTUnwrap(sink.read()), try bytes(.notificationAction(id: 2, revision: 2, key: "current")))
        XCTAssertEqual(try XCTUnwrap(sink.read()), try bytes(.notificationClosed(id: 2, revision: 2, reason: .dismissed)))
        XCTAssertEqual(try XCTUnwrap(sink.read()), try bytes(.notificationClosed(id: 65, revision: 1, reason: .undefined)))
        XCTAssertNil(sink.read(timeout: 0.02)); XCTAssertEqual(sink.failures, 0)
    }
}
