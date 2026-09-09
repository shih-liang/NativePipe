import AppKit
import XCTest
import NativePipeProtocol
@testable import NativePipeRemote

final class RemoteApplicationTests: XCTestCase {
    func testCatalogBinaryReplyAndLimits() throws {
        var payload = ApplicationReply.magic + Data([1, 1, 0, 0])
        func number(_ value: UInt32) { var v = value.littleEndian; withUnsafeBytes(of: &v) { payload.append(contentsOf: $0) } }
        func field(_ bytes: Data) { number(UInt32(bytes.count)); payload.append(bytes) }
        number(42); number(1)
        for value in ["editor.desktop", "Éditeur", "Text editor", "editor", "Editor", "editor-icon"] { field(Data(value.utf8)) }
        guard case .batch(let token, let apps) = try ApplicationReply.decode(payload) else {
            return XCTFail("Expected catalog")
        }
        XCTAssertEqual(token, 42)
        XCTAssertEqual(apps.first?.name, "Éditeur")
        XCTAssertEqual(apps.first?.iconName, "editor-icon")
        XCTAssertThrowsError(try ApplicationReply.decode(payload.dropLast()))
        XCTAssertThrowsError(try ApplicationReply.decode(payload + Data([0])))
        var command = SSHCommand(destination: "host", application: [])
        XCTAssertThrowsError(try command.validate())
        command.persistentSession = true
        XCTAssertNoThrow(try command.validate())
        XCTAssertTrue(command.remoteScript.contains("--stdio --session"))
    }
    func testCredentialRetryAttemptIsPerSessionAndPrompt() throws {
        let directory = try SSHCredentialStore.makeAttemptDirectory(environment: [:])
        defer { try? FileManager.default.removeItem(at: directory) }
        let attributes = try FileManager.default.attributesOfItem(atPath: directory.path)
        XCTAssertEqual((attributes[.posixPermissions] as? NSNumber)?.intValue, 0o700)
        XCTAssertTrue(SSHCredentialStore.claimCachedAttempt(prompt: "password:", directory: directory.path))
        XCTAssertFalse(SSHCredentialStore.claimCachedAttempt(prompt: "password:", directory: directory.path))
        XCTAssertTrue(SSHCredentialStore.claimCachedAttempt(prompt: "key passphrase:", directory: directory.path))
    }
    func testKeychainRoundTrip() throws {
        guard ProcessInfo.processInfo.environment["NATIVEPIPE_TEST_KEYCHAIN"] == "1" else {
            throw XCTSkip("Opt-in test writes only a disposable test credential")
        }
        let store = SSHCredentialStore(connection: "nativepipe-test-" + UUID().uuidString)
        let prompt = "test password:"
        defer { try? store.remove(prompt) }
        try store.save("first", prompt: prompt)
        XCTAssertEqual(try store.read(prompt), "first")
        try store.save("second", prompt: prompt)
        XCTAssertEqual(try store.read(prompt), "second")
        try store.remove(prompt)
        XCTAssertNil(try store.read(prompt))
    }
    @MainActor func testLiveRemoteImmediateExit() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let destination = env["NATIVEPIPE_TEST_REMOTE"],
              let compositor = env["NATIVEPIPE_TEST_COMPOSITOR"] else {
            throw XCTSkip("Requires an explicitly selected remote test machine")
        }
        for (program, status) in [("false", Int32(1)), ("true", Int32(0))] {
            let session = RemoteSession(command: .init(destination: destination, application: [program],
                sshArguments: ["-o", "BatchMode=yes"], compositor: compositor))
            let connecting = Task { try await session.connect() }
            let timeout = Task { try? await Task.sleep(for: .seconds(10)); connecting.cancel() }
            _ = try? await connecting.value
            timeout.cancel()
            let deadline = Date().addingTimeInterval(5)
            while session.exitStatus == nil && Date() < deadline {
                try await Task.sleep(for: .milliseconds(50))
            }
            XCTAssertEqual(session.exitStatus, status, "The application exit must terminate SSH")
            session.disconnect()
        }
    }
    @MainActor func testLiveRemoteCatalogLaunchAndCapture() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let destination = env["NATIVEPIPE_TEST_REMOTE"],
              let compositor = env["NATIVEPIPE_TEST_COMPOSITOR"] else {
            throw XCTSkip("Requires an explicitly selected remote test machine")
        }
        _ = NSApplication.shared
        var command = SSHCommand(destination: destination, application: [],
                                 sshArguments: ["-o", "BatchMode=yes"], compositor: compositor)
        command.persistentSession = true
        let display = RemoteDisplayController(command: command)
        display.session.onDiagnostic = { fputs($0, stderr) }
        defer { display.disconnect() }
        try await display.connect()
        let applications = try await display.session.applications()
        XCTAssertFalse(applications.isEmpty)
        let demo = try XCTUnwrap(applications.first { $0.executable.contains("gtk4-demo") })
        try await display.session.launchApplication(demo.id)
        let deadline = Date().addingTimeInterval(20)
        while display.bridge.dockWindows.isEmpty && Date() < deadline {
            try await Task.sleep(for: .milliseconds(50))
        }
        let window = try XCTUnwrap(display.bridge.dockWindows.first)
        XCTAssertGreaterThan(window.width, 0)
        XCTAssertTrue(display.bridge.activateDockWindow(window.id))
        try await Task.sleep(for: .seconds(2))
        let capture = try await display.bridge.captureDockWindow(window.id, maximumWidth: 1200, maximumHeight: 900)
        XCTAssertGreaterThan(capture.png.count, 3000)
        if let output = env["NATIVEPIPE_TEST_CAPTURE"] {
            try capture.png.write(to: URL(fileURLWithPath: output))
        }
        XCTAssertTrue(display.bridge.requestCloseDockWindow(window.id))
        let closing = Date().addingTimeInterval(5)
        while !display.bridge.dockWindows.isEmpty && Date() < closing {
            try await Task.sleep(for: .milliseconds(50))
        }
        XCTAssertTrue(display.bridge.dockWindows.isEmpty)
    }
}
