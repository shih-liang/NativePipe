import Foundation
@testable import NativePipeNotifications
import XCTest

final class GuestNotificationContentTests: XCTestCase {
    private func content(title: String = "T", body: String = "B", actions: [GuestNotificationContent.Action] = [],
                         expiry: TimeInterval? = nil) -> GuestNotificationContent {
        .init(title: title, subtitle: "Arch · Terminal", body: body, actions: actions, expiry: expiry, quiet: false)
    }

    func testAnythingTheGuestPolicyProducesFits() {
        let longest = content(title: String(repeating: "t", count: GuestNotificationLimits.title),
                              body: String(repeating: "b", count: GuestNotificationLimits.body),
                              actions: (0..<GuestNotificationLimits.actions).map {
                                  .init(key: "k\($0)", label: String(repeating: "l", count: GuestNotificationLimits.actionLabel))
                              }, expiry: 3600)
        XCTAssertTrue(longest.isWithinLimits)
    }

    func testAnotherProcessCannotExceedTheLimits() {
        XCTAssertFalse(content(title: String(repeating: "t", count: GuestNotificationLimits.title + 1)).isWithinLimits)
        XCTAssertFalse(content(body: String(repeating: "b", count: GuestNotificationLimits.body + 1)).isWithinLimits)
        XCTAssertFalse(content(actions: (0...GuestNotificationLimits.actions).map { .init(key: "k\($0)", label: "L") }).isWithinLimits)
        XCTAssertFalse(content(actions: [.init(key: "", label: "L")]).isWithinLimits)
        XCTAssertFalse(content(actions: [.init(key: String(repeating: "k", count: GuestNotificationLimits.actionKey + 1), label: "L")]).isWithinLimits)
        XCTAssertFalse(content(expiry: 3601).isWithinLimits)
        XCTAssertFalse(content(expiry: -1).isWithinLimits)
    }

    func testResponsesNeedAGuestIdentifierAndBoundedAction() {
        let id = GuestNotificationLimits.identifierPrefix + "m.s.1.1.x"
        XCTAssertTrue(GuestNotificationResponse(identifier: id, action: nil).isValid)
        XCTAssertTrue(GuestNotificationResponse(identifier: id, action: "open").isValid)
        XCTAssertFalse(GuestNotificationResponse(identifier: "com.apple.something", action: nil).isValid)
        XCTAssertFalse(GuestNotificationResponse(identifier: id + String(repeating: "x", count: 300), action: nil).isValid)
        XCTAssertFalse(GuestNotificationResponse(identifier: id,
            action: String(repeating: "a", count: GuestNotificationLimits.actionKey + 1)).isValid)
    }

    func testActionCategoriesAreStableAndDistinguishLabels() {
        let a = [GuestNotificationContent.Action(key: "open", label: "Open")]
        let b = [GuestNotificationContent.Action(key: "open", label: "Show")]
        XCTAssertEqual(GuestNotificationCategories.identifier(a), GuestNotificationCategories.identifier(a))
        XCTAssertNotEqual(GuestNotificationCategories.identifier(a), GuestNotificationCategories.identifier(b))
    }

    func testForwardingRefusesSocketsOutsideTheConfiguredDirectory() async {
        let id = GuestNotificationLimits.identifierPrefix + "m.s.1.1.x"
        let allowed = GuestNotificationResponseForwarder.endpointDirectory(in: URL(fileURLWithPath: "/tmp/npa"))
        for path in ["/tmp/other/n0123456789abcdef", "/tmp/npa/.n/not-a-socket-name", "/tmp/npa/.n/../n0123456789abcdef"] {
            let delivered = await GuestNotificationResponseForwarder.forward(.init(identifier: id, action: nil),
                path: path, permittedDirectories: [allowed])
            XCTAssertFalse(delivered, path)
        }
    }
}
