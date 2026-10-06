import Foundation
import XCTest
@testable import NativePipeRemote

final class SFTPTransferTests: XCTestCase {
    @MainActor func testCancellingTransferOrAuthenticationClosesAttemptAndAllowsRetry() async throws {
        let files = FileManager.default
        let directory = files.temporaryDirectory.appendingPathComponent("nativepipe-sftp-cancel-" + UUID().uuidString)
        try files.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? files.removeItem(at: directory) }
        let executable = directory.appendingPathComponent("ssh-fixture")
        // /usr/bin/sftp uses a local SSH fixture. It never contacts a host or
        // sends a file; an open authentication prompt keeps stderr alive.
        try Data("""
        #!/bin/sh
        printf '%s' "$NATIVEPIPE_SSH_AUTH_SESSION" > "$TEST_AUTH_READY"
        if [ "$TEST_CANCEL_AUTH" = 1 ]; then
            rmdir "$NATIVEPIPE_SSH_AUTH_SESSION"
        else
            count=0
            while [ -d "$NATIVEPIPE_SSH_AUTH_SESSION" ] && [ "$count" -lt 60 ]; do
                /bin/sleep 0.05
                count=$((count + 1))
            done
        fi
        printf 'NATIVEPIPE AUTH CANCELLED\\n' >&2
        exit 1
        """.utf8).write(to: executable)
        try files.setAttributes([.posixPermissions: 0o700], ofItemAtPath: executable.path)
        let command = SSHCommand(destination: "unused.invalid", application: ["true"], sshArguments: ["-S", executable.path])
        let transfer = SFTPTransfer()
        for cancelPrompt in [false, true] {
            let ready = directory.appendingPathComponent(UUID().uuidString)
            let task = Task {
                try await transfer.run(command: command, direction: .upload, local: directory.appendingPathComponent("unused"),
                    remote: "unused", environment: ["TEST_AUTH_READY": ready.path, "TEST_CANCEL_AUTH": cancelPrompt ? "1" : "0"])
            }
            // Bounds only a broken run: a working one continues the moment the
            // stand-in starts. Two seconds was too tight for spawning sftp and
            // the shell fixture while the rest of the suite loads the machine,
            // and on expiry the read below failed with a misleading "no such
            // file" instead of saying what actually never happened.
            let deadline = ContinuousClock.now + .seconds(15)
            while !files.fileExists(atPath: ready.path), ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(10)) }
            guard files.fileExists(atPath: ready.path) else {
                XCTFail("The SSH stand-in never reached its authentication prompt (cancelPrompt=\(cancelPrompt))")
                transfer.cancel()
                _ = try? await task.value
                return
            }
            let path = try String(contentsOf: ready, encoding: .utf8)
            if !cancelPrompt {
                transfer.cancel()
                XCTAssertFalse(files.fileExists(atPath: path), "Cancellation must close the prompt before waiting for SFTP to exit")
            }
            do { try await task.value; XCTFail("Cancellation must not be success") }
            catch is CancellationError { }
            XCTAssertFalse(files.fileExists(atPath: path))
        }
    }

    @MainActor func testLiteralPathsCannotInjectCommands() throws {
        let batch = try SFTPTransfer.batch(direction: .upload, local: "/tmp/a b*[1]\".txt", remote: "-target")
        XCTAssertTrue(batch.hasPrefix("put -- \"/tmp/a b*[1]\\\".txt\" \"-target\""))
        XCTAssertEqual(batch.split(separator: "\n").count, 2)
        for path in ["", "file\n!touch /tmp/unwanted", "file\rbye", "file\0"] {
            XCTAssertThrowsError(try SFTPTransfer.batch(direction: .download, local: "/tmp/file", remote: path))
        }
    }

    @MainActor func testRemoteFileRoundTrip() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let destination = env["NATIVEPIPE_TEST_REMOTE"] else {
            throw XCTSkip("Set NATIVEPIPE_TEST_REMOTE for the opt-in SSH file round trip.")
        }
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("nativepipe-sftp-test-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let source = directory.appendingPathComponent("file \\ *[1] \"quote\".txt")
        let target = directory.appendingPathComponent("downloaded.txt")
        let data = Data("NativePipe SFTP round trip\n".utf8)
        try data.write(to: source)
        // The unique remote temp file is removed by the test's normal SSH cleanup.
        let remote = "/tmp/nativepipe-sftp-" + UUID().uuidString + " *[1].txt"
        var command = SSHCommand(destination: destination, application: [])
        command.persistentSession = true
        let transfer = SFTPTransfer()
        defer {
            let cleanup = Process()
            cleanup.executableURL = URL(fileURLWithPath: "/usr/bin/ssh")
            cleanup.arguments = ["-o", "BatchMode=yes", "--", destination, "rm -f -- " + SSHCommand.quote(remote)]
            if (try? cleanup.run()) != nil { cleanup.waitUntilExit() }
        }
        try await transfer.run(command: command, direction: .upload, local: source, remote: remote, environment: env)
        try await transfer.run(command: command, direction: .download, local: target, remote: remote, environment: env)
        XCTAssertEqual(try Data(contentsOf: target), data)

        let folder = directory.appendingPathComponent("folder")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)
        try data.write(to: folder.appendingPathComponent("child.txt"))
        let files = RemoteUserFileAccess(command: command, environment: env)
        let imported = try await files.importFiles([folder], shareDirectories: false)
        let importedRoot = try XCTUnwrap(imported.first).deletingLastPathComponent().path
        XCTAssertTrue(importedRoot.hasPrefix("/tmp/nativepipe-drop-"))
        defer {
            let cleanup = Process()
            cleanup.executableURL = URL(fileURLWithPath: "/usr/bin/ssh")
            cleanup.arguments = ["-o", "BatchMode=yes", "--", destination, "rm -r -- " + SSHCommand.quote(importedRoot)]
            if (try? cleanup.run()) != nil { cleanup.waitUntilExit() }
        }
        let copied = directory.appendingPathComponent("copied")
        try await files.exportFile(imported[0], to: copied)
        XCTAssertEqual(try Data(contentsOf: copied.appendingPathComponent("child.txt")), data)
        do { try await files.exportFile(imported[0], to: copied); XCTFail("must not overwrite") } catch { }
    }

    @MainActor func testEarlySSHFailureDoesNotKillTheHostWithSIGPIPE() async {
        let command = SSHCommand(destination: "unused", application: ["true"],
                                 sshArguments: ["-o", "NativePipeInvalidOption=yes"])
        do {
            try await SFTPTransfer().run(command: command, direction: .upload,
                local: URL(fileURLWithPath: "/tmp/unused"), remote: "unused", environment: [:])
            XCTFail("Invalid SSH option must fail")
        } catch { XCTAssertTrue(error.localizedDescription.contains("Bad configuration option")) }
    }
}
