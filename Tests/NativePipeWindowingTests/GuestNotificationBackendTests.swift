import Foundation
import NativePipeProtocol
import XCTest
@testable import NativePipeWindowing

@MainActor
private final class RecordingBackend: GuestNotificationBackend {
    var state: GuestNotificationAuthorization = .authorized
    var prompts: [Bool] = []
    var presented: [(identifier: String, path: String)] = []
    var withdrawn: [String] = []
    var attachments: [(directory: URL?, hasResponder: Bool)] = []
    var failure: Error?

    func authorization(prompt: Bool) async -> GuestNotificationAuthorization {
        prompts.append(prompt)
        return state
    }
    func present(identifier: String, content: GuestNotificationContent, responsePath: String) async throws {
        if let failure { throw failure }
        presented.append((identifier, responsePath))
    }
    func withdraw(identifier: String) { withdrawn.append(identifier) }
    func attach(owner: UUID, directory: URL?, responder: ((String, String?) -> Bool)?) {
        attachments.append((directory, responder != nil))
    }
}

/// The in-process center is the same whether macOS is reached directly or
/// through the resident service: only the backend differs.
@MainActor
final class GuestNotificationBackendTests: XCTestCase {
    private var directory: URL!
    private let content = GuestNotificationContent(title: "T", subtitle: "S", body: "B", actions: [],
                                                   expiry: nil, quiet: false)

    override func setUpWithError() throws {
        // sockaddr_un limits the path length: keep it short.
        directory = URL(fileURLWithPath: "/tmp/npb" + UUID().uuidString.prefix(6))
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
    }
    override func tearDown() { try? FileManager.default.removeItem(at: directory) }

    func testPresentationCarriesTheResponseSocketAndWithdrawalIsForwarded() async throws {
        let backend = RecordingBackend()
        let center = SystemGuestNotificationCenter(responseDirectory: directory, backend: backend)
        center.onResponse = { _, _ in true }
        XCTAssertEqual(backend.attachments.last?.hasResponder, true)
        let route = try XCTUnwrap(backend.attachments.last?.directory)
        XCTAssertEqual(route, GuestNotificationResponseRouter.endpointDirectory(in: directory))

        try await center.present(identifier: "nativepipe.guest.a", content: content)
        let path = try XCTUnwrap(backend.presented.first?.path)
        XCTAssertEqual(URL(fileURLWithPath: path).deletingLastPathComponent().standardizedFileURL, route.standardizedFileURL)
        center.withdraw(identifier: "nativepipe.guest.a")
        XCTAssertEqual(backend.withdrawn, ["nativepipe.guest.a"])

        center.onResponse = nil
        XCTAssertEqual(backend.attachments.last?.hasResponder, false)
    }

    func testAskingIsDeferredToTheBackendAndOnlyDeliverableStatesAllowDelivery() async {
        let backend = RecordingBackend()
        let center = SystemGuestNotificationCenter(responseDirectory: directory, backend: backend)
        let withoutRoute = await center.authorize()
        XCTAssertFalse(withoutRoute, "no response route means actions would be lost")
        XCTAssertTrue(backend.prompts.isEmpty)

        center.onResponse = { _, _ in true }
        for (state, allowed) in [(GuestNotificationAuthorization.authorized, true), (.alertsOff, true),
                                 (.denied, false), (.notDetermined, false)] {
            backend.state = state
            let result = await center.authorize()
            XCTAssertEqual(result, allowed, "\(state)")
        }
        XCTAssertEqual(backend.prompts, [true, true, true, true])
    }

    func testBackendFailureIsReportedToThePresenter() async throws {
        let backend = RecordingBackend()
        backend.failure = POSIXError(.EPIPE)
        let center = SystemGuestNotificationCenter(responseDirectory: directory, backend: backend)
        center.onResponse = { _, _ in true }
        do { try await center.present(identifier: "nativepipe.guest.a", content: content); XCTFail("must throw") }
        catch { XCTAssertTrue(backend.presented.isEmpty) }
    }

    func testContentSurvivesTheWireBetweenProcesses() throws {
        let value = GuestNotificationContent(title: "Build", subtitle: "Arch · Terminal", body: "done",
            actions: [.init(key: "open", label: "Open")], expiry: 12, quiet: true)
        XCTAssertEqual(try JSONDecoder().decode(GuestNotificationContent.self, from: JSONEncoder().encode(value)), value)
    }

    func testRouterTrustsOnlyItsOwnBundleAndNamedForwarders() {
        XCTAssertFalse(GuestNotificationResponseRouter.allowsForwarder(bundle: "com.example.service"))
        XCTAssertFalse(GuestNotificationResponseRouter.allowsForwarder(bundle: nil))
        GuestNotificationForwarding.trust(bundle: "com.example.service")
        XCTAssertTrue(GuestNotificationResponseRouter.allowsForwarder(bundle: "com.example.service"))
        XCTAssertFalse(GuestNotificationResponseRouter.allowsForwarder(bundle: "com.example.other"))
    }

    func testAuthorizationStatesDeliverExceptDeniedAndUndecided() {
        XCTAssertEqual(GuestNotificationAuthorization.allCases, [.notDetermined, .authorized, .denied, .alertsOff])
        XCTAssertEqual(GuestNotificationAuthorization.allCases.filter(\.allowsDelivery), [.authorized, .alertsOff])
    }
}
