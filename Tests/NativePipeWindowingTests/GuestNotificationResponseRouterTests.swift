import Darwin
import Foundation
import NativePipeProtocol
import XCTest
@testable import NativePipeWindowing

@MainActor
final class GuestNotificationResponseRouterTests: XCTestCase {
    private func temporaryDirectory() throws -> URL {
        let url = URL(fileURLWithPath: "/tmp").appendingPathComponent("np-notify-" + UUID().uuidString.prefix(8))
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        return url
    }

    func testRoutesOnlyCurrentIdentifierAndAllowedAction() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        var received: [String] = []
        let router = try GuestNotificationResponseRouter(directory: root) { identifier, action in
            guard identifier == "nativepipe.guest.current", action == "open" else { return false }
            received.append(identifier); return true
        }
        defer { router.stop() }
        let response = GuestNotificationResponseRouter.Response(identifier: "nativepipe.guest.current", action: "open")
        let accepted = await GuestNotificationResponseRouter.forward(response, path: router.url.path, permittedDirectories: [router.directory])
        XCTAssertTrue(accepted)
        let stale = await GuestNotificationResponseRouter.forward(.init(identifier: "nativepipe.guest.old", action: "open"),
            path: router.url.path, permittedDirectories: [router.directory])
        let unknown = await GuestNotificationResponseRouter.forward(.init(identifier: response.identifier, action: "unknown"),
            path: router.url.path, permittedDirectories: [router.directory])
        XCTAssertFalse(stale); XCTAssertFalse(unknown)
        XCTAssertEqual(received, [response.identifier])
        router.stop()
        XCTAssertFalse(FileManager.default.fileExists(atPath: router.url.path))
        let stopped = await GuestNotificationResponseRouter.forward(response, path: router.url.path, permittedDirectories: [router.directory])
        XCTAssertFalse(stopped)
    }

    func testRejectsUnpermittedDirectoryAndOversizedContent() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        var calls = 0
        let router = try GuestNotificationResponseRouter(directory: root) { _, _ in calls += 1; return true }
        defer { router.stop() }
        let wrongDirectory = await GuestNotificationResponseRouter.forward(.init(identifier: "nativepipe.guest.current", action: nil),
            path: router.url.path, permittedDirectories: [])
        let huge = await GuestNotificationResponseRouter.forward(.init(identifier: "nativepipe.guest.current", action: String(repeating: "a", count: 129)),
            path: router.url.path, permittedDirectories: [router.directory])
        XCTAssertFalse(wrongDirectory); XCTAssertFalse(huge); XCTAssertEqual(calls, 0)
    }

    func testRejectsFrameBeforeAllocatingOversizedPayload() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        var calls = 0
        let router = try GuestNotificationResponseRouter(directory: root) { _, _ in calls += 1; return true }
        defer { router.stop() }
        let connection = try await SocketConnection.connect(to: router.url)
        defer { connection.close() }
        try await connection.write(WireFormat.encodeHeader(payloadCount: GuestNotificationResponseRouter.maximumPayload + 1))
        let response = try await connection.read(upToCount: 1, deadline: .now() + .seconds(3))
        XCTAssertTrue(response.isEmpty); XCTAssertEqual(calls, 0)
    }

    func testRefusesSymlinkOrPublicResponseDirectory() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let directory = root.appendingPathComponent(".n")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o755])
        XCTAssertThrowsError(try GuestNotificationResponseRouter(directory: root) { _, _ in true })
        try FileManager.default.removeItem(at: directory)
        try FileManager.default.createSymbolicLink(at: directory, withDestinationURL: root)
        XCTAssertThrowsError(try GuestNotificationResponseRouter(directory: root) { _, _ in true })
    }
}
