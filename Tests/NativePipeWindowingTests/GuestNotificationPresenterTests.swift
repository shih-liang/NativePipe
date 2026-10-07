import Foundation
import NativePipeProtocol
import XCTest
import UserNotifications
@testable import NativePipeWindowing

@MainActor
private final class FakeCenter: GuestNotificationCenter {
    var onResponse: ((String, String?) -> Bool)?
    var allowed = true
    var authorizations = 0
    var presented: [(identifier: String, content: GuestNotificationContent)] = []
    var withdrawn: [String] = []
    var failure: Error?
    var visible: [String: GuestNotificationContent] = [:]
    var holdPresentation = false
    var pendingPresentations: [(String, CheckedContinuation<Void, Never>)] = []
    var holdAuthorization = false
    var pendingAuthorizations: [CheckedContinuation<Bool, Never>] = []

    func authorize() async -> Bool {
        authorizations += 1
        if holdAuthorization { return await withCheckedContinuation { pendingAuthorizations.append($0) } }
        return allowed
    }
    func present(identifier: String, content: GuestNotificationContent) async throws {
        if holdPresentation {
            await withCheckedContinuation { pendingPresentations.append((identifier, $0)) }
        }
        if let failure { throw failure }
        presented.append((identifier, content))
        visible[identifier] = content
    }
    func withdraw(identifier: String) { withdrawn.append(identifier); visible[identifier] = nil }
}

@MainActor
final class GuestNotificationPresenterTests: XCTestCase {
    private var center: FakeCenter!
    private var sent: [Windowing.HostCommand] = []
    private var enabled = true
    private var clock: TimeInterval = 0

    override func setUp() {
        center = FakeCenter(); sent = []; enabled = true; clock = 0
    }

    private func makePresenter(limiter: GuestNotificationRateLimiter = .init()) -> GuestNotificationPresenter {
        GuestNotificationPresenter(
            machine: "Dev", center: center, isEnabled: { [unowned self] in enabled },
            limiter: limiter, now: { [unowned self] in clock },
            send: { [unowned self] in sent.append($0) })
    }

    private func settle() async {
        for _ in 0..<20 { await Task.yield() }
    }

    private func waitUntil(_ condition: () -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + 2
        while !condition() {
            guard ProcessInfo.processInfo.systemUptime < deadline else {
                XCTFail("Notification lifecycle did not reach the expected state")
                throw CancellationError()
            }
            try await Task.sleep(for: .milliseconds(2))
        }
    }

    private func note(
        _ id: UInt32 = 1, revision: UInt64 = 1, summary: String = "Build finished", body: String = "All tests passed",
        app: String = "Terminal", actions: [Windowing.GuestNotification.Action] = [],
        timeout: Int = -1, urgency: Windowing.GuestNotification.Urgency = .normal
    ) -> Windowing.GuestNotification {
        .init(id: id, revision: revision, urgency: urgency, timeoutMilliseconds: timeout, appName: app,
              summary: summary, body: body, actions: actions)
    }

    // MARK: Policy

    func testContentNamesTheMachineAndApplication() throws {
        let content = try XCTUnwrap(GuestNotificationPolicy.content(for: note(), machine: "Dev"))
        XCTAssertEqual(content.title, "Build finished")
        XCTAssertEqual(content.subtitle, "Dev · Terminal")
        XCTAssertEqual(content.body, "All tests passed")
        XCTAssertNil(content.expiry)
        XCTAssertFalse(content.quiet)
        let anonymous = try XCTUnwrap(GuestNotificationPolicy.content(for: note(app: ""), machine: "Dev"))
        XCTAssertEqual(anonymous.subtitle, "Dev")
    }

    func testMarkupEntitiesAndControlCharactersAreStripped() throws {
        let content = try XCTUnwrap(GuestNotificationPolicy.content(
            for: note(summary: "<b>Bold</b> &amp; <a href=\"http://evil\">link</a>\u{7}",
                      body: "line one\r\nline two\t\t<img src=\"x\"/> end \u{202E}"),
            machine: "Dev"))
        XCTAssertEqual(content.title, "Bold & link")
        XCTAssertEqual(content.body, "line one\nline two end")
    }

    func testLengthsAreBounded() throws {
        let content = try XCTUnwrap(GuestNotificationPolicy.content(
            for: note(summary: String(repeating: "t", count: 500), body: String(repeating: "b", count: 5_000)),
            machine: "Dev"))
        XCTAssertEqual(content.title.count, GuestNotificationPolicy.maximumTitleLength)
        XCTAssertTrue(content.title.hasSuffix("…"))
        XCTAssertEqual(content.body.count, GuestNotificationPolicy.maximumBodyLength)
    }

    func testEmptyNotificationIsDroppedAndBodyOnlyBecomesTheTitle() throws {
        XCTAssertNil(GuestNotificationPolicy.content(for: note(summary: " <b></b> ", body: "\u{7}"), machine: "Dev"))
        let bodyOnly = try XCTUnwrap(GuestNotificationPolicy.content(for: note(summary: "", body: "Only text"), machine: "Dev"))
        XCTAssertEqual(bodyOnly.title, "Only text")
        XCTAssertEqual(bodyOnly.body, "")
    }

    func testActionsExcludeTheDefaultDuplicatesAndAreLimited() throws {
        let actions = (["default", "a", "a", "b", "", "c", "d", "e", "f"]).map {
            Windowing.GuestNotification.Action(key: $0, label: "Label \($0)")
        }
        let content = try XCTUnwrap(GuestNotificationPolicy.content(for: note(actions: actions), machine: "Dev"))
        XCTAssertEqual(content.actions.map(\.key), ["a", "b", "c", "d"], "The banner click is 'default', not a button.")
        let unlabeled = try XCTUnwrap(GuestNotificationPolicy.content(
            for: note(actions: [.init(key: "x", label: "  ")]), machine: "Dev"))
        XCTAssertTrue(unlabeled.actions.isEmpty)
    }

    func testExpiryAndUrgency() throws {
        XCTAssertEqual(try XCTUnwrap(GuestNotificationPolicy.content(for: note(timeout: 5000), machine: "Dev")).expiry, 5)
        XCTAssertEqual(try XCTUnwrap(GuestNotificationPolicy.content(for: note(timeout: 99_999_999), machine: "Dev")).expiry,
                       GuestNotificationPolicy.maximumExpiry)
        XCTAssertNil(try XCTUnwrap(GuestNotificationPolicy.content(for: note(timeout: 0), machine: "Dev")).expiry, "0 means never")
        XCTAssertTrue(try XCTUnwrap(GuestNotificationPolicy.content(for: note(urgency: .low), machine: "Dev")).quiet)
    }

    func testRateLimiterAllowsABurstThenRefills() {
        var limiter = GuestNotificationRateLimiter(capacity: 3, refillInterval: 2)
        XCTAssertEqual((0..<5).map { _ in limiter.allow(now: 0) }, [true, true, true, false, false])
        XCTAssertFalse(limiter.allow(now: 1))
        XCTAssertTrue(limiter.allow(now: 2.5), "One token returns every two seconds.")
        XCTAssertFalse(limiter.allow(now: 2.5))
        XCTAssertEqual((0..<4).map { _ in limiter.allow(now: 1_000) }, [true, true, true, false],
                       "Idle time never banks more than the burst.")
    }

    // MARK: Presenter

    func testPostPresentsOnceAfterAskingForPermission() async throws {
        let presenter = makePresenter()
        presenter.post(note(1)); presenter.post(note(2, summary: "Second"))
        await settle()
        XCTAssertEqual(center.authorizations, 2)
        XCTAssertEqual(center.presented.map(\.identifier), [presenter.identifier(for: 1), presenter.identifier(for: 2)].compactMap { $0 })
        XCTAssertEqual(presenter.activeIdentifiers, [1, 2])
    }

    func testDisabledOrDeniedNotificationsAreNotShown() async {
        enabled = false
        let presenter = makePresenter()
        presenter.post(note(1))
        await settle()
        XCTAssertTrue(center.presented.isEmpty)
        XCTAssertEqual(center.authorizations, 0, "A disabled feature must not trigger the system prompt.")

        enabled = true; center.allowed = false
        presenter.post(note(2)); presenter.post(note(3))
        await settle()
        XCTAssertTrue(center.presented.isEmpty)
        XCTAssertEqual(center.authorizations, 2, "The center checks current settings; it alone merges an initial system prompt.")
    }

    func testFloodingIsRateLimited() async {
        let presenter = makePresenter(limiter: .init(capacity: 2, refillInterval: 60))
        for id in 1...10 { presenter.post(note(UInt32(id))) }
        await settle()
        XCTAssertEqual(center.presented.count, 2)
    }

    func testClickRunsTheDefaultActionAndClosesTheNotification() async throws {
        let presenter = makePresenter()
        presenter.post(note(7)); await settle()
        _ = center.onResponse?(try XCTUnwrap(presenter.identifier(for: 7)), GuestNotificationPolicy.defaultActionKey)
        guard case .notificationAction(let id, let revision, let key) = sent.first else { return XCTFail("no action: \(sent)") }
        XCTAssertEqual(id, 7); XCTAssertEqual(revision, 1); XCTAssertEqual(key, "default")
        guard case .notificationClosed(7, 1, .dismissed) = sent.last else { return XCTFail("not closed: \(sent)") }
        XCTAssertTrue(presenter.activeIdentifiers.isEmpty)
    }

    func testChosenActionIsReportedWithItsKey() async throws {
        let presenter = makePresenter()
        presenter.post(note(3, actions: [.init(key: "archive", label: "Archive")])); await settle()
        _ = center.onResponse?(try XCTUnwrap(presenter.identifier(for: 3)), "archive")
        guard case .notificationAction(3, 1, "archive") = sent.first else { return XCTFail("\(sent)") }
    }

    func testDismissingSendsOnlyTheClose() async throws {
        let presenter = makePresenter()
        presenter.post(note(4)); await settle()
        _ = center.onResponse?(try XCTUnwrap(presenter.identifier(for: 4)), nil)
        XCTAssertEqual(sent.count, 1)
        guard case .notificationClosed(4, 1, .dismissed) = sent[0] else { return XCTFail("\(sent)") }
    }

    func testResponsesForUnknownIdentifiersAreIgnored() async {
        let presenter = makePresenter()
        presenter.post(note(5)); await settle()
        for identifier in ["guest-99", "other-5", "guest-", "guest-abc", "guest--1"] {
            _ = center.onResponse?(identifier, "default")
        }
        XCTAssertTrue(sent.isEmpty, "A stale or forged identifier must not reach the guest.")
        XCTAssertEqual(presenter.activeIdentifiers, [5])
    }

    func testGuestClosingWithdrawsWithoutEchoing() async throws {
        let presenter = makePresenter()
        presenter.post(note(6)); await settle()
        let identifier = try XCTUnwrap(presenter.identifier(for: 6))
        presenter.close(id: 6, revision: 1)
        XCTAssertEqual(center.withdrawn, [identifier])
        XCTAssertTrue(sent.isEmpty)
        presenter.close(id: 6, revision: 1)
        XCTAssertEqual(center.withdrawn, [identifier], "Closing twice withdraws once.")
    }

    func testExpiryWithdrawsAndTellsTheGuest() async throws {
        let presenter = makePresenter()
        presenter.post(note(8, timeout: 30)); await settle()
        let identifier = try XCTUnwrap(presenter.identifier(for: 8))
        try await Task.sleep(for: .milliseconds(300))
        XCTAssertEqual(center.withdrawn, [identifier])
        guard case .notificationClosed(8, 1, .expired) = sent.first else { return XCTFail("\(sent)") }
        XCTAssertTrue(presenter.activeIdentifiers.isEmpty)
    }

    func testFailedPresentationLeavesNothingTracked() async {
        struct Failure: Error {}
        center.failure = Failure()
        let presenter = makePresenter()
        presenter.post(note(9)); await settle()
        XCTAssertTrue(presenter.activeIdentifiers.isEmpty)
    }

    func testLatePresentationCannotResurrectGuestClosedNotification() async throws {
        center.holdPresentation = true
        let presenter = makePresenter()
        presenter.post(note())
        try await waitUntil { self.center.pendingPresentations.count == 1 }
        let (_, pending) = center.pendingPresentations.removeFirst()
        presenter.close(id: 1, revision: 1)
        pending.resume()
        try await waitUntil { self.center.presented.count == 1 }
        await settle()
        XCTAssertTrue(center.visible.isEmpty)
        XCTAssertTrue(presenter.activeIdentifiers.isEmpty)
        XCTAssertTrue(sent.isEmpty, "Guest closure must not be echoed by a late add")
    }

    func testLateReplacementCannotOverwriteOrRemoveNewNotification() async throws {
        center.holdPresentation = true
        let presenter = makePresenter()
        presenter.post(note(summary: "Old"))
        try await waitUntil { self.center.pendingPresentations.count == 1 }
        let (oldIdentifier, pending) = center.pendingPresentations.removeFirst()
        center.holdPresentation = false
        presenter.post(note(revision: 2, summary: "New"))
        try await waitUntil { self.center.presented.count == 1 }
        let newIdentifier = try XCTUnwrap(presenter.identifier(for: 1))
        XCTAssertNotEqual(oldIdentifier, newIdentifier)
        pending.resume()
        try await waitUntil { self.center.presented.count == 2 }
        await settle()
        XCTAssertEqual(Set(center.visible.keys), [newIdentifier])
        XCTAssertEqual(center.visible[newIdentifier]?.title, "New")
        _ = center.onResponse?(oldIdentifier, "default")
        XCTAssertTrue(sent.isEmpty)
        XCTAssertEqual(presenter.activeIdentifiers, [1])
    }

    func testOldAddFailureCannotDiscardSuccessfulReplacement() async throws {
        struct Failure: Error {}
        center.holdPresentation = true
        let presenter = makePresenter()
        presenter.post(note(summary: "Old"))
        try await waitUntil { self.center.pendingPresentations.count == 1 }
        let (_, pending) = center.pendingPresentations.removeFirst()
        center.holdPresentation = false
        presenter.post(note(revision: 2, summary: "New"))
        try await waitUntil { self.center.presented.count == 1 }
        center.failure = Failure()
        pending.resume()
        await settle()
        XCTAssertEqual(presenter.activeIdentifiers, [1])
        XCTAssertEqual(center.visible.values.first?.title, "New")
        XCTAssertTrue(sent.isEmpty)
    }

    func testStopDuringAuthorizationPreventsPresentationAndResetStartsNewSession() async throws {
        center.holdAuthorization = true
        let presenter = makePresenter()
        presenter.post(note())
        let oldIdentifier = try XCTUnwrap(presenter.identifier(for: 1))
        try await waitUntil { self.center.pendingAuthorizations.count == 1 }
        presenter.stop()
        center.pendingAuthorizations.removeFirst().resume(returning: true)
        await settle()
        XCTAssertTrue(center.presented.isEmpty)
        XCTAssertTrue(presenter.activeIdentifiers.isEmpty)
        presenter.post(note())
        XCTAssertTrue(presenter.activeIdentifiers.isEmpty)
        presenter.resetSession()
        center.holdAuthorization = false
        presenter.post(note())
        try await waitUntil { self.center.presented.count == 1 }
        XCTAssertNotEqual(oldIdentifier, presenter.identifier(for: 1))
    }

    func testDeniedPermissionIsRecheckedAndLeavesNoPendingRecord() async throws {
        center.allowed = false
        let presenter = makePresenter()
        presenter.post(note())
        await settle()
        XCTAssertTrue(presenter.activeIdentifiers.isEmpty)
        center.allowed = true
        presenter.post(note(2))
        try await waitUntil { self.center.presented.count == 1 }
        XCTAssertEqual(center.authorizations, 2)
    }

    func testPreferenceRevocationWithdrawsPendingAndDeliveredNotifications() async throws {
        let presenter = makePresenter()
        presenter.post(note())
        try await waitUntil { self.center.presented.count == 1 }
        enabled = false
        presenter.refreshPreferences()
        XCTAssertTrue(center.visible.isEmpty)
        XCTAssertTrue(presenter.activeIdentifiers.isEmpty)
        enabled = true
        presenter.post(note(2))
        try await waitUntil { self.center.presented.count == 2 }
    }

    func testMachineAndSessionNamespacesCannotCollideForSameGuestID() async throws {
        let first = makePresenter(), secondCenter = FakeCenter()
        let second = GuestNotificationPresenter(machine: "Other", identity: "another-machine", center: secondCenter, send: { _ in })
        first.post(note()); second.post(note())
        let a = try XCTUnwrap(first.identifier(for: 1)), b = try XCTUnwrap(second.identifier(for: 1))
        XCTAssertNotEqual(a, b)
        first.resetSession(); first.post(note())
        XCTAssertNotEqual(a, first.identifier(for: 1))
    }

    func testActionCategoriesIncludeLabelsAndCannotHaveDelimiterCollisions() {
        let original: [GuestNotificationContent.Action] = [.init(key: "open", label: "Open")]
        let relabeled: [GuestNotificationContent.Action] = [.init(key: "open", label: "Run")]
        let stable = SystemGuestNotificationCenter.categoryIdentifier(original)
        XCTAssertEqual(Set((0..<100).map { _ in SystemGuestNotificationCenter.categoryIdentifier(original) }), [stable])
        XCTAssertNotEqual(SystemGuestNotificationCenter.categoryIdentifier(original), SystemGuestNotificationCenter.categoryIdentifier(relabeled))
        let a: [GuestNotificationContent.Action] = [.init(key: "a|b", label: "A"), .init(key: "c", label: "B")]
        let b: [GuestNotificationContent.Action] = [.init(key: "a", label: "A"), .init(key: "b|c", label: "B")]
        XCTAssertNotEqual(SystemGuestNotificationCenter.categoryIdentifier(a), SystemGuestNotificationCenter.categoryIdentifier(b))
    }

    func testSystemReservedAndControlCharacterActionKeysAreNotRegistered() throws {
        let content = try XCTUnwrap(GuestNotificationPolicy.content(for: note(actions: [
            .init(key: UNNotificationDefaultActionIdentifier, label: "Default"),
            .init(key: UNNotificationDismissActionIdentifier, label: "Dismiss"),
            .init(key: "invalid\nkey", label: "Invalid"),
            .init(key: "valid", label: "Open"),
        ]), machine: "Dev"))
        XCTAssertEqual(content.actions, [.init(key: "valid", label: "Open")])
    }

    func testCategoryRegistrationPreservesOtherOwnersAndPrunesUnusedGuestCategories() {
        let foreign = UNNotificationCategory(identifier: "host-feature", actions: [], intentIdentifiers: [], options: [])
        let oldActions: [GuestNotificationContent.Action] = [.init(key: "open", label: "Open")]
        let oldID = SystemGuestNotificationCenter.categoryIdentifier(oldActions)
        let old = UNNotificationCategory(identifier: oldID, actions: [], intentIdentifiers: [], options: [])
        let unused = UNNotificationCategory(identifier: "nativepipe.guest.actions.unused", actions: [], intentIdentifiers: [], options: [])
        let newActions: [GuestNotificationContent.Action] = [.init(key: "open", label: "Run")]
        let result = SystemGuestNotificationCenter.mergeCategories([foreign, old, unused], used: [oldID], actions: newActions)
        XCTAssertEqual(Set(result.map(\.identifier)), ["host-feature", oldID, SystemGuestNotificationCenter.categoryIdentifier(newActions)])
        XCTAssertEqual(result.first(where: { $0.identifier == SystemGuestNotificationCenter.categoryIdentifier(newActions) })?.actions.first?.title, "Run")
    }

    func testSuppressedAndFailedNotificationsReleaseGuestIDs() async throws {
        let presenter = makePresenter(limiter: .init(capacity: 3, refillInterval: 60))
        enabled = false
        presenter.post(note(1))
        enabled = true
        presenter.post(note(2, summary: "", body: ""))
        center.allowed = false
        presenter.post(note(3))
        await settle()
        center.allowed = true
        struct Failure: Error {}
        center.failure = Failure()
        presenter.post(note(4))
        await settle()
        center.failure = nil
        presenter.post(note(5)) // Empty/refused/failed posts exhausted the burst.
        let closed = sent.compactMap { command -> UInt32? in
            if case .notificationClosed(let id, _, .undefined) = command { return id }
            return nil
        }
        XCTAssertEqual(Set(closed), [1, 2, 3, 4, 5])
        XCTAssertTrue(presenter.activeIdentifiers.isEmpty)
    }

    func testStopReleasesEveryGuestIDButNewSessionResetDoesNotEchoOldIDs() async throws {
        let presenter = makePresenter()
        presenter.post(note(1)); presenter.post(note(2))
        try await waitUntil { self.center.presented.count == 2 }
        presenter.stop()
        XCTAssertEqual(sent.count, 2)
        XCTAssertEqual(Set(sent.compactMap { command -> UInt32? in
            if case .notificationClosed(let id, _, .undefined) = command { return id }
            return nil
        }), [1, 2])
        sent = []
        presenter.resetSession()
        presenter.post(note(3))
        presenter.resetSession()
        XCTAssertTrue(sent.isEmpty, "old IDs must not be sent on a replacement guest session")
    }

    func testActionKeysArePreservedAndUnregisteredResponsesAreRejected() async throws {
        let presenter = makePresenter()
        presenter.post(note(actions: [.init(key: " archive ", label: "Archive")]))
        try await waitUntil { self.center.presented.count == 1 }
        XCTAssertEqual(center.presented.first?.content.actions.first?.key, " archive ")
        let identifier = try XCTUnwrap(presenter.identifier(for: 1))
        _ = center.onResponse?(identifier, "unregistered")
        XCTAssertTrue(sent.isEmpty)
        _ = center.onResponse?(identifier, " archive ")
        guard case .notificationAction(1, 1, " archive ") = sent.first else { return XCTFail("original key was changed") }
    }

    func testActiveNotificationTrackingIsBounded() async {
        let presenter = makePresenter(limiter: .init(capacity: 100, refillInterval: 60))
        for id in 1...100 { presenter.post(note(UInt32(id))) }
        XCTAssertEqual(presenter.activeIdentifiers.count, GuestNotificationPresenter.maximumActive)
        presenter.stop()
    }
    func testStaleRevisionCannotWithdrawReplacementOrRunItsAction() async throws {
        let presenter = makePresenter()
        presenter.post(note()); await settle()
        let old = try XCTUnwrap(presenter.identifier(for: 1))
        presenter.post(note(revision: 2, summary: "Replacement")); await settle()
        let current = try XCTUnwrap(presenter.identifier(for: 1))
        presenter.close(id: 1, revision: 1)
        XCTAssertFalse(presenter.handleResponse(identifier: old, action: "default"))
        XCTAssertEqual(presenter.identifier(for: 1), current)
        XCTAssertEqual(center.visible[current]?.title, "Replacement")
        XCTAssertTrue(sent.isEmpty)
        _ = presenter.handleResponse(identifier: current, action: "default")
        guard case .notificationAction(1, 2, "default") = sent.first,
              case .notificationClosed(1, 2, .dismissed) = sent.last else { return XCTFail("Wrong revision") }
    }

    func testPresenterIsReleasedDuringPendingAddAndLongExpiry() async throws {
        center.holdPresentation = true
        var presenter: GuestNotificationPresenter? = makePresenter()
        weak var weakPresenter = presenter
        presenter?.post(note(timeout: 3_600_000))
        try await waitUntil { self.center.pendingPresentations.count == 1 }
        presenter = nil
        XCTAssertNil(weakPresenter, "The presentation task must not retain its owner")
        center.pendingPresentations.removeFirst().1.resume()
        await settle()
        XCTAssertTrue(center.visible.isEmpty)
        center.holdPresentation = false
        presenter = makePresenter(); weakPresenter = presenter
        presenter?.post(note(timeout: 3_600_000))
        try await waitUntil { !self.center.visible.isEmpty }
        presenter = nil
        XCTAssertNil(weakPresenter, "The expiry task must not retain its owner")
        await settle()
        XCTAssertTrue(center.visible.isEmpty)
    }

    func testBodyOnlyTitleAndBacklogResetAreBounded() async throws {
        let content = try XCTUnwrap(GuestNotificationPolicy.content(
            for: note(summary: "", body: String(repeating: "b", count: 5000)), machine: "Dev"))
        XCTAssertEqual(content.title.count, GuestNotificationPolicy.maximumTitleLength)
        let presenter = makePresenter()
        presenter.post(note(revision: 50)); await settle()
        presenter.resetBacklog()
        XCTAssertTrue(presenter.activeIdentifiers.isEmpty)
        guard case .notificationClosed(1, 50, .undefined) = sent.last else { return XCTFail("Wrong reset revision") }
    }

}
