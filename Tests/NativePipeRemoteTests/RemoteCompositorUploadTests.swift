import CryptoKit
import XCTest
import NativePipeProtocol
@testable import NativePipeRemote

final class RemoteCompositorUploadTests: XCTestCase {
    @MainActor func testTargetSelectionUploadAndCachedProtocolHandoff() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        var payload = Data(WindowWire.lifecycleMagic + [1, 1, 0, 0, 42, 0, 0, 0])
        var version = WindowWire.windowProtocolVersion.littleEndian
        withUnsafeBytes(of: &version) { payload.append(contentsOf: $0) }
        let ready = try WireFormat.frame(payload: payload).base64EncodedString()
        for (index, target) in ["aarch64 gnu", "aarch64 musl", "x86_64 gnu", "x86_64 musl"].enumerated() {
            let data = Data(repeating: UInt8(index + 1), count: 1_048_601 + index)
            try data.write(to: directory.appendingPathComponent("nativepipe-compositor-\(target.replacingOccurrences(of: " ", with: "-")).tar.gz"))
            let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
            for cached in [false, true] {
                // The peer independently verifies target bytes/checksum. A
                // cached reply and binary ready record arrive in one write.
                let peer = """
                import sys, os, hashlib, base64
                os.write(1, b'NATIVEPIPE TARGET \(target)\\n')
                digest, size = sys.stdin.buffer.readline().split()
                assert digest.decode() == '\(digest)' and int(size) == \(data.count)
                if \(cached ? "False" : "True"):
                    os.write(1, b'NATIVEPIPE UPLOAD\\n')
                    data = sys.stdin.buffer.read(int(size))
                    assert len(data) == int(size) and hashlib.sha256(data).hexdigest() == digest.decode()
                    os.write(1, base64.b64decode('\(ready)'))
                else:
                    os.write(1, b'NATIVEPIPE CACHED\\n' + base64.b64decode('\(ready)'))
                sys.stdin.buffer.read()
                """
                let session = RemoteSession(testExecutable: "/usr/bin/python3", arguments: ["-c", peer],
                                            localCompositorDirectory: directory)
                let task = Task { try await session.connect() }
                let deadline = Task { try? await Task.sleep(for: .seconds(5)); task.cancel() }
                defer { deadline.cancel(); session.disconnect() }
                try await task.value
                XCTAssertTrue(session.isConnected)
            }
        }
    }

    @MainActor func testFailedPreludeDrainsDiagnosticsAndCanBeCancelled() async throws {
        let directory = FileManager.default.temporaryDirectory
        let message = "NativePipe bootstrap: unsupported CPU"
        let failed = RemoteSession(testExecutable: "/bin/sh",
            arguments: ["-c", "printf '%s' \(SSHCommand.quote(message)) >&2; exit 3"],
            localCompositorDirectory: directory)
        do { try await failed.connect(); XCTFail("Expected failure") }
        catch { XCTAssertEqual(error.localizedDescription, message) }
        XCTAssertFalse(failed.isConnected)
        let stalled = RemoteSession(testExecutable: "/bin/sleep", arguments: ["30"], localCompositorDirectory: directory)
        let task = Task { try await stalled.connect() }
        try await Task.sleep(for: .milliseconds(30))
        task.cancel()
        do { try await task.value; XCTFail("Expected cancellation") }
        catch is CancellationError { }
        XCTAssertFalse(stalled.isConnected)
    }

    @MainActor func testUntrustedTargetCannotEscapeTheBundle() async throws {
        for response in ["NATIVEPIPE TARGET ../../tmp gnu\n", String(repeating: "X", count: 257)] {
            let session = RemoteSession(testExecutable: "/bin/sh",
                arguments: ["-c", "printf '%s' \(SSHCommand.quote(response)); read reply"],
                localCompositorDirectory: FileManager.default.temporaryDirectory)
            do { try await session.connect(); XCTFail("Expected invalid prelude") }
            catch { XCTAssertTrue(error.localizedDescription.contains("response")) }
            XCTAssertFalse(session.isConnected)
        }
    }
}
