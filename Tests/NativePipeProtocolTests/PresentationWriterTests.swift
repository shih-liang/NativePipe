import XCTest
@testable import NativePipeProtocol

final class PresentationWriterTests: XCTestCase {
    private final class Sink: @unchecked Sendable {
        let entered = DispatchSemaphore(value: 0), resume = DispatchSemaphore(value: 0)
        private let condition = NSCondition()
        private var first = true
        private var packets: [Data] = []
        func write(_ bytes: Data) {
            condition.lock(); let block = first; first = false; condition.unlock()
            if block { entered.signal(); _ = resume.wait(timeout: .now() + 5) }
            condition.lock(); packets.append(bytes); condition.broadcast(); condition.unlock()
        }
        func read() -> Data? {
            condition.lock(); defer { condition.unlock() }
            let deadline = Date().addingTimeInterval(2)
            while packets.isEmpty { if !condition.wait(until: deadline) { return nil } }
            return packets.removeFirst()
        }
    }

    func testOutcomeAndDrainCannotBeBatchedAsBufferReleaseOrOvertakeEachOther() throws {
        let sink = Sink()
        let writer = WindowCommandWriter(lane: .feedback) { _, bytes in sink.write(bytes) }
        writer.install(try FileHandle(forWritingTo: URL(fileURLWithPath: "/dev/null")))
        defer { sink.resume.signal(); writer.disconnect() }
        let first = Windowing.HostCommand.frameReleased(surface: 2, presentationID: 1)
        writer.send(first)
        XCTAssertEqual(sink.entered.wait(timeout: .now() + 2), .success)
        let releases: [Windowing.HostCommand] = [
            .frameReleased(surface: 2, presentationID: 2), .frameReleased(surface: 2, presentationID: 3)
        ]
        let sample = Windowing.HostCommand.sceneClockSample(sessionID: 42, clockEpoch: 3, surface: 2,
            presentationID: 3, guestSendTimeNanoseconds: 100, hostReceiveTimeNanoseconds: 200)
        let result = Windowing.HostCommand.presentationFeedback(sessionID: 42, clockEpoch: 3,
            surface: 2, presentationID: 3, hostTimeNanoseconds: 300, refreshNanoseconds: 0, outputID: 0)
        let drain = Windowing.HostCommand.presentationDrain(sessionID: 42, token: 7)
        for command in releases + [sample, result, drain, first] { writer.send(command) }
        sink.resume.signal()
        XCTAssertEqual(try XCTUnwrap(sink.read()), try WireFormat.frame(
            payload: XCTUnwrap(WindowWire.frameTimingPayload(for: [first][...]))))
        XCTAssertEqual(try XCTUnwrap(sink.read()), try WireFormat.frame(
            payload: XCTUnwrap(WindowWire.frameTimingPayload(for: releases[...]))))
        for command in [sample, result, drain] {
            XCTAssertEqual(try XCTUnwrap(sink.read()), try WireFormat.frame(payload: WindowWire.commandPayload(for: command)))
        }
        XCTAssertEqual(try XCTUnwrap(sink.read()), try WireFormat.frame(
            payload: XCTUnwrap(WindowWire.frameTimingPayload(for: [first][...]))))
    }
}
