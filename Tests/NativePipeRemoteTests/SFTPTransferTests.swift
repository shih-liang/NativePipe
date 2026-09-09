import Foundation
import XCTest
@testable import NativePipeRemote

final class SFTPTransferTests: XCTestCase {
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
