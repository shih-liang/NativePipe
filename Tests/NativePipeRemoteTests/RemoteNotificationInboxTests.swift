import XCTest
import NativePipeProtocol
@testable import NativePipeRemote

final class RemoteNotificationInboxTests: XCTestCase {
    @MainActor func testNotificationFloodPreservesScenesAndMediaWithoutSpendingStructuralBudget() throws {
        let writer = WindowCommandWriter(remote: true) { _, _ in }
        let pipe = Pipe()
        writer.install(pipe.fileHandleForWriting)
        defer { writer.disconnect() }
        var events: [Windowing.GuestEvent] = []
        var failures = 0
        let media = DispatchSemaphore(value: 0), read = DispatchSemaphore(value: 0)
        let inbound = RemoteInbound(writer: writer, media: { _, _ in media.signal(); return true }) { event in
            switch event {
            case .packet(.event(let event)): events.append(event)
            case .failed: failures += 1
            default: break
            }
        }
        let header = MediaWire.Header(surfaceID: 1, resourceID: 1, width: 1,
            height: 1, ptsNanos: 0, payloadLength: 1)
        DispatchQueue.global().async {
            inbound.receive(.event(.channelReady(sessionID: 1, protocolVersion: WindowWire.windowProtocolVersion)), bytes: 16)
            for revision in UInt64(1)...10_000 {
                inbound.receive(.event(.notificationPosted(.init(id: 1,
                    revision: revision, summary: "Newest \(revision)"))), bytes: 64 * 1024 * 1024)
                if revision <= 100 {
                    inbound.receive(.event(.frameCallbackRequested(surface: 1,
                        presentationID: UInt32(revision))), bytes: 16)
                    inbound.receive(.event(.committed(surface: 1, frame: .init(
                        resourceID: UInt32(revision), width: 1, height: 1,
                        bytesPerRow: 4, format: .bgra8888,
                        presentationID: UInt32(revision)))), bytes: 64)
                }
                inbound.receive(.event(.notificationClosed(id: UInt32(revision + 1_000), revision: 1)), bytes: 20)
            }
            inbound.receive(.media(header, Data([42])), bytes: 1)
            read.signal()
        }
        XCTAssertEqual(read.wait(timeout: .now() + 5), .success)
        XCTAssertEqual(media.wait(timeout: .now()), .success, "Media admission must bypass the blocked main actor")
        XCTAssertEqual(events.count, 0)
        let deadline = Date().addingTimeInterval(2)
        while events.isEmpty || !events.contains(where: { if case .notificationPosted = $0 { return true }; return false }) {
            if Date() >= deadline { break }
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        XCTAssertEqual(failures, 0)
        guard case .channelReady = events.first else { return XCTFail("Handshake must precede notifications") }
        let frames = events.compactMap { event -> UInt32? in
            if case .frameCallbackRequested(_, let id) = event { return id }
            return nil
        }
        XCTAssertEqual(frames, Array(UInt32(1)...100), "Every presentation lease must survive in wire order")
        let commits = events.compactMap { event -> UInt32? in
            if case .committed(_, let frame) = event { return frame.presentationID }
            return nil
        }
        XCTAssertEqual(commits, frames, "Frame commits cannot be replaced along with optional notifications")
        let posted = events.compactMap { event -> Windowing.GuestNotification? in
            if case .notificationPosted(let note) = event { return note }
            return nil
        }
        XCTAssertEqual(posted.count, 1)
        XCTAssertEqual(posted.first?.revision, 10_000)
        XCTAssertLessThanOrEqual(events.count, 201 + GuestNotificationInbox.maximumPendingCount)
        inbound.stop()
    }

    @MainActor func testStructuralOverflowStillFailsOnceAndStopDiscardsOptionalState() {
        let writer = WindowCommandWriter { _, _ in }
        let pipe = Pipe()
        writer.install(pipe.fileHandleForWriting)
        defer { writer.disconnect() }
        var failureCount = 0, notificationCount = 0
        let inbound = RemoteInbound(writer: writer, media: nil) { event in
            if case .failed = event { failureCount += 1 }
            if case .packet(.event(.notificationPosted)) = event { notificationCount += 1 }
        }
        inbound.receive(.event(.notificationPosted(.init(id: 1, summary: "Discard on failure"))), bytes: 4)
        for id in UInt32(1)...4096 {
            XCTAssertTrue(inbound.receive(.event(.frameCallbackRequested(surface: 1, presentationID: id)), bytes: 16))
        }
        XCTAssertFalse(inbound.receive(.event(.surfaceDestroyed(surface: 1)), bytes: 12))
        XCTAssertFalse(inbound.receive(.event(.notificationPosted(.init(id: 2, summary: "Old reader"))), bytes: 4))
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        XCTAssertEqual(failureCount, 1)
        XCTAssertEqual(notificationCount, 0)

        let next = RemoteInbound(writer: writer, media: nil) { event in
            if case .packet(.event(.notificationPosted)) = event { notificationCount += 1 }
        }
        next.receive(.event(.notificationPosted(.init(id: 1, summary: "Next connection"))), bytes: 4)
        next.stop()
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        XCTAssertEqual(notificationCount, 0, "Stop must clear the entire optional inbox")
    }

    func testMalformedOptionalContentDoesNotPreventTheFollowingFrameButBadFramingStillFails() throws {
        func integer<T: FixedWidthInteger>(_ value: T, _ data: inout Data) {
            var value = value.littleEndian
            withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
        }
        var ready = Data(WindowWire.lifecycleMagic + [1, 1, 0, 0])
        integer(UInt32(1), &ready); integer(WindowWire.windowProtocolVersion, &ready)
        var badNote = Data(WindowWire.lifecycleMagic + [1, 38, 0, 0])
        integer(UInt32(7), &badNote); integer(UInt64(9), &badNote)
        badNote.append(3) // invalid urgency inside a correctly framed optional record
        var surface = Data(WindowWire.lifecycleMagic + [1, 2, 0, 0])
        integer(UInt32(42), &surface)
        var decoder = RemoteStreamDecoder()
        decoder.append(try WireFormat.frame(payload: ready) + WireFormat.frame(payload: badNote)
            + WireFormat.frame(payload: surface))
        guard case .event(.channelReady) = try decoder.next() else { return XCTFail("Missing handshake") }
        guard case .event(.notificationRejected(id: 7?, revision: 9?)) = try decoder.next() else {
            return XCTFail("Optional content must produce one bounded rejection")
        }
        guard case .event(.surfaceCreated(surface: 42)) = try decoder.next() else {
            return XCTFail("Following structural state must remain readable")
        }
        try decoder.finish()
        decoder.append(Data("broken frame".utf8))
        XCTAssertThrowsError(try decoder.next(), "True framing errors remain fatal")
    }
}
