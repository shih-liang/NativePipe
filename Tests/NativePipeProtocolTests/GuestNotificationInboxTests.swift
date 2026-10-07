import XCTest
@testable import NativePipeProtocol

final class GuestNotificationInboxTests: XCTestCase {
    func testReplacementsCoalesceWithoutLosingTheNewestRevision() {
        var inbox = GuestNotificationInbox()
        for revision in 1...20_000 {
            XCTAssertTrue(inbox.offer(.notificationPosted(.init(id: 1,
                revision: UInt64(revision), summary: "Revision \(revision)"))))
        }
        inbox.offer(.notificationClosed(id: 1, revision: 19_999))
        inbox.offer(.notificationRejected(id: 1, revision: 19_999))
        XCTAssertEqual(inbox.pendingCount, 1)
        let events = inbox.drain()
        guard events.count == 1, case .notificationPosted(let note) = events[0] else {
            return XCTFail("Only the latest replacement should survive")
        }
        XCTAssertEqual(note.revision, 20_000)
        XCTAssertEqual(note.summary, "Revision 20000")
        XCTAssertTrue(inbox.isEmpty)
    }

    func testCloseChurnCannotEvictReliableRejections() {
        var inbox = GuestNotificationInbox()
        for id in UInt32(1)...64 { inbox.offer(.notificationRejected(id: id, revision: 2)) }
        for id in UInt32(1_000)...21_000 {
            inbox.offer(.notificationClosed(id: id, revision: 1))
            XCTAssertLessThanOrEqual(inbox.pendingCount, GuestNotificationInbox.maximumPendingCount)
        }
        let events = inbox.drain()
        XCTAssertEqual(events.filter { if case .notificationBacklogReset = $0 { return true }; return false }.count, 1)
        let rejected = events.compactMap { event -> UInt32? in
            if case .notificationRejected(let id, let revision) = event {
                XCTAssertEqual(revision, 2)
                return id
            }
            return nil
        }
        XCTAssertEqual(rejected, Array(UInt32(1)...64))
        XCTAssertLessThanOrEqual(events.count, GuestNotificationInbox.maximumPendingCount)
        XCTAssertTrue(inbox.isEmpty)
    }

    func testFullGuestRegistryCanChurnThenDeliverAllRemainingActiveNotifications() {
        var inbox = GuestNotificationInbox()
        for round in 0..<100 {
            for offset in UInt32(1)...64 {
                let id = UInt32(round * 64) + offset
                inbox.offer(.notificationPosted(.init(id: id, summary: "Closed soon")))
                inbox.offer(.notificationClosed(id: id, revision: 1))
            }
        }
        for id in UInt32(10_000)...10_063 {
            inbox.offer(.notificationPosted(.init(id: id, revision: 2, summary: "Active")))
        }
        let events = inbox.drain()
        let posted = events.compactMap { event -> UInt32? in
            if case .notificationPosted(let note) = event { return note.id }
            return nil
        }
        XCTAssertEqual(posted, Array(UInt32(10_000)...10_063))
        XCTAssertEqual(events.filter { if case .notificationBacklogReset = $0 { return true }; return false }.count, 1)
        XCTAssertLessThanOrEqual(events.count, GuestNotificationInbox.maximumPendingCount)
    }

    func testNormalCloseAndRejectionKeepRevisionSemanticsWithoutResettingOtherBanners() {
        var inbox = GuestNotificationInbox()
        inbox.offer(.notificationPosted(.init(id: 1, revision: 1, summary: "Old")))
        inbox.offer(.notificationRejected(id: 1, revision: 2))
        inbox.offer(.notificationPosted(.init(id: 2, revision: 1, summary: "Other")))
        inbox.offer(.notificationClosed(id: 2, revision: 1))
        let events = inbox.drain()
        XCTAssertEqual(events.count, 2)
        guard case .notificationClosed(id: 2, revision: 1) = events[0],
              case .notificationRejected(id: 1?, revision: 2?) = events[1] else {
            return XCTFail("An ordinary close must not reset unrelated banners")
        }
        inbox.offer(.notificationClosed(id: 1, revision: 2))
        inbox.offer(.notificationPosted(.init(id: 1, revision: 2, summary: "Stale")))
        inbox.offer(.notificationPosted(.init(id: 1, revision: 3, summary: "New")))
        let replacement = inbox.drain()
        guard replacement.count == 1, case .notificationPosted(let note) = replacement[0] else {
            return XCTFail("A later replacement should supersede the pending close")
        }
        XCTAssertEqual(note.revision, 3)
    }

    func testNonconformingUnknownIDsStayBoundedAndUnidentifiedRejectionsProduceNoReplies() {
        var inbox = GuestNotificationInbox()
        for id in UInt32(1)...20_000 {
            inbox.offer(.notificationPosted(.init(id: id, summary: "Unknown excess ID")))
            inbox.offer(.notificationRejected(id: nil, revision: nil))
            XCTAssertLessThanOrEqual(inbox.pendingCount, GuestNotificationInbox.maximumPendingCount)
        }
        let events = inbox.drain()
        XCTAssertLessThanOrEqual(events.count, GuestNotificationInbox.maximumPendingCount)
        XCTAssertEqual(events.filter { if case .notificationPosted = $0 { return true }; return false }.count, 64)
        XCTAssertEqual(events.filter { if case .notificationRejected = $0 { return true }; return false }.count, 64)
        XCTAssertEqual(events.filter { if case .notificationBacklogReset = $0 { return true }; return false }.count, 1)
        XCTAssertTrue(inbox.isEmpty)
    }

    func testSessionInboxesAreIndependentAndClearDiscardsEveryOldGenerationEvent() {
        var first = GuestNotificationInbox(), second = GuestNotificationInbox()
        first.offer(.notificationPosted(.init(id: 1, summary: "First machine")))
        second.offer(.notificationPosted(.init(id: 1, summary: "Second machine")))
        first.offer(.notificationBacklogReset)
        first.clear()
        XCTAssertTrue(first.drain().isEmpty)
        XCTAssertEqual(second.pendingCount, 1)
        let event = second.drain().first
        guard case .notificationPosted(let note) = event else { return XCTFail("Missing second machine") }
        XCTAssertEqual(note.summary, "Second machine")
        XCTAssertFalse(first.offer(.frameCallbackRequested(surface: 7, presentationID: 3)))
        XCTAssertFalse(first.offer(.surfaceDestroyed(surface: 7)))
        XCTAssertTrue(first.isEmpty)
    }
}
