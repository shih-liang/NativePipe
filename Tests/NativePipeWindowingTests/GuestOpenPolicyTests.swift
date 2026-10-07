import Foundation
import NativePipeProtocol
import XCTest
@testable import NativePipeWindowing

final class GuestOpenPolicyTests: XCTestCase {
    func testSchemesCredentialsAndMailAttachmentsAreDecidedByTheUser() {
        for text in ["https://alice:secret@example.com/report", "mailto:me@example.com?attach=/tmp/report.pdf",
                     "ssh://example.com", "custom:action"] {
            XCTAssertEqual(GuestOpenPolicy.decide(.init(kind: .url, value: text), shares: []), .openURL(URL(string: text)!))
        }
        let display = GuestOpenPolicy.displayURL(URL(string: "https://alice:secret@example.com/report")!)
        XCTAssertFalse(display.contains("alice")); XCTAssertFalse(display.contains("secret"))
        XCTAssertTrue(display.contains("example.com"))
    }
    func testFileURLsCannotBypassGuestTransferAndShareRevocation() {
        let request = HostOpenWire.Request(kind: .url, value: "file:///mnt/linportal/revoked/app.command")
        guard case .refuse = GuestOpenPolicy.decide(request, shares: []) else { return XCTFail("File URL bypassed revoked share") }
        XCTAssertEqual(GuestOpenPolicy.decide(.init(kind: .url, value: "file:///home/me/tool.pkg"), shares: []),
                       .receiveFile(path: "/home/me/tool.pkg", name: "tool.pkg", share: nil))
        guard case .refuse = GuestOpenPolicy.decide(.init(kind: .url, value: "file://different-machine/path"), shares: []) else {
            return XCTFail("File URL used a different authority")
        }
    }

    func testMalformedRequestsAreStillRefused() {
        for text in ["https://", "not a URL", "https://example.com\nspoof"] {
            guard case .refuse = GuestOpenPolicy.decide(.init(kind: .url, value: text), shares: []) else {
                return XCTFail("Accepted malformed URL")
            }
        }
        for path in ["relative", "/", "/a/../b", "/a/./b", "/a\0b"] {
            guard case .refuse = GuestOpenPolicy.decide(.init(kind: .file, value: path), shares: []) else {
                return XCTFail("Accepted malformed path")
            }
        }
    }

    func testSharesRequireCurrentAuthorizationAndAlwaysUseTransfer() {
        let share = GuestOpenPolicy.SharedFolder(tag: "docs", hostRoot: URL(fileURLWithPath: "/tmp/documents"))
        let request = HostOpenWire.Request(kind: .file, value: "/mnt/linportal/docs/test.command")
        XCTAssertEqual(GuestOpenPolicy.decide(request, shares: [share]),
            .receiveFile(path: request.value, name: "test.command", share: share))
        guard case .refuse = GuestOpenPolicy.decide(request, shares: []) else { return XCTFail("Revoked share accepted") }
        guard case .refuse = GuestOpenPolicy.decide(.init(kind: .file, value: "/mnt//linportal/revoked/file"), shares: []) else {
            return XCTFail("Repeated path separators bypassed share revocation")
        }
        XCTAssertEqual(GuestOpenPolicy.decide(.init(kind: .file, value: "/home/me/tool.pkg"), shares: []),
            .receiveFile(path: "/home/me/tool.pkg", name: "tool.pkg", share: nil))
    }

    func testExecutableContentWithAnInnocentExtensionNeedsConfirmation() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let script = root.appendingPathComponent("notes.txt")
        try Data("#!/bin/sh\necho test".utf8).write(to: script)
        XCTAssertTrue(try GuestOpenPolicy.mayExecute(script))
        XCTAssertFalse(try GuestOpenPolicy.mayExecute(root), "Ordinary directories are not programs")
    }
}
