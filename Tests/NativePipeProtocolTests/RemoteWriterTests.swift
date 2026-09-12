import Foundation
import NativePipeProtocol
import XCTest

final class RemoteWriterTests: XCTestCase {
    private final class Sink: @unchecked Sendable {
        let condition = NSCondition()
        var packets: [Data] = []
        func write(_ bytes: Data) {
            condition.lock(); packets.append(bytes); condition.signal(); condition.unlock()
        }
        func read(timeout: TimeInterval = 2) -> Data? {
            condition.lock(); defer { condition.unlock() }
            let deadline = Date().addingTimeInterval(timeout)
            while packets.isEmpty {
                if !condition.wait(until: deadline) { return nil }
            }
            return packets.removeFirst()
        }
    }

    func testKeysAndCreditsBypassBlockedClipboardWithoutLosingBytes() throws {
        let sink = Sink()
        let writer = WindowCommandWriter(remote: true) { _, bytes in sink.write(bytes) }
        writer.install(try FileHandle(forWritingTo: URL(fileURLWithPath: "/dev/null")))
        defer { writer.disconnect() }
        let bytes = Data((0..<300_000).map { UInt8(truncatingIfNeeded: $0) })
        let clipboard = Windowing.HostCommand.hostSelectionData(token: 7, mimeType: "text/plain", data: bytes)
        let expected = try WireFormat.frame(payload: WindowWire.commandPayload(for: clipboard))
        writer.send(clipboard)
        var received = RemoteWire.Reassembler()
        var total = 0
        for _ in 0..<4 {
            let record = try XCTUnwrap(sink.read())
            let fragment = Data(record.dropFirst(WireFormat.headerSize))
            XCTAssertNil(try received.receive(fragment, maximumSize: 400_000))
            total += fragment.count - 16
        }
        XCTAssertEqual(total, RemoteWire.initialWindowSize)
        XCTAssertNil(sink.read(timeout: 0.05), "Bulk must stop even when the local pipe is writable")
        let down = Windowing.HostCommand.key(window: 1, keycode: 30, pressed: true, modifiers: [])
        let up = Windowing.HostCommand.key(window: 1, keycode: 30, pressed: false, modifiers: [])
        writer.send(down); writer.send(up)
        XCTAssertEqual(try XCTUnwrap(sink.read()), try WireFormat.frame(payload: WindowWire.commandPayload(for: down)))
        XCTAssertEqual(try XCTUnwrap(sink.read()), try WireFormat.frame(payload: WindowWire.commandPayload(for: up)))
        writer.acknowledgeRemoteBytes(16384, received: true)
        XCTAssertEqual(try XCTUnwrap(sink.read()), try WireFormat.frame(payload: RemoteWire.acknowledgement(16384)))
        writer.remotePresentation(surface: 1, presentationID: 9, displayed: true, intervalNanoseconds: 16_666_667)
        let presentation = Data(try XCTUnwrap(sink.read()).dropFirst(WireFormat.headerSize))
        XCTAssertEqual(presentation.count, 20)
        XCTAssertEqual(presentation.prefix(4), Data("NPRP".utf8))
        XCTAssertEqual(RemoteWire.number(presentation, at: 12), 1)
        XCTAssertEqual(RemoteWire.number(presentation, at: 16), 16_666_667)
        writer.acknowledgeRemoteBytes(total, received: false)
        while total < expected.count {
            let fragment = Data(try XCTUnwrap(sink.read()).dropFirst(WireFormat.headerSize))
            let record = try received.receive(fragment, maximumSize: 400_000)
            let count = fragment.count - 16
            total += count
            writer.acknowledgeRemoteBytes(count, received: false)
            if let record { XCTAssertEqual(record, expected) }
        }
        XCTAssertTrue(received.isEmpty)
    }

    func testReceiveBudgetShrinksWithQueueDelayAndRecovers() {
        var flow = RemoteWire.FlowControl()
        var now = 1.0
        func roundtrip(_ duration: Double) {
            while case let count = flow.allowance(remaining: 1_000_000), count > 0 {
                flow.sent(count, now: now)
            }
            let bytes = flow.bytes
            now += duration
            XCTAssertTrue(flow.acknowledge(bytes, now: now))
        }
        for _ in 0..<10 { roundtrip(0.1) }
        let fast = flow.window
        XCTAssertGreaterThanOrEqual(fast, 262_144) // >= 20 Mbit/s at 100 ms RTT
        XCTAssertLessThanOrEqual(fast, 1_048_576)
        for _ in 0..<16 { roundtrip(0.5) }
        XCTAssertLessThan(flow.window, fast / 8)
        for _ in 0..<10 { roundtrip(0.13) }
        XCTAssertGreaterThan(flow.window, 4096)
        XCTAssertFalse(flow.acknowledge(1, now: now))
    }
}
