import XCTest
import NativePipeProtocol
@testable import NativePipeRemote

final class NativePipeRemoteTests: XCTestCase {
    private func ready() throws -> Data {
        var payload = Data(WindowWire.lifecycleMagic + [1, 1, 0, 0])
        var pid: UInt32 = 123
        var version = WindowWire.windowProtocolVersion.littleEndian
        withUnsafeBytes(of: &pid) { payload.append(contentsOf: $0) }
        withUnsafeBytes(of: &version) { payload.append(contentsOf: $0) }
        return try WireFormat.frame(payload: payload)
    }
    func testMixedStreamOneByteAtATime() throws {
        let header = MediaWire.Header(surfaceID: 1, resourceID: 2,
            width: 4, height: 4, ptsNanos: 0, payloadLength: 3)
        let stream = try ready() + header.encoded() + Data([1, 2, 3])
        var decoder = RemoteStreamDecoder(), events = 0, media = 0
        for byte in stream {
            decoder.append(Data([byte]))
            while let packet = try decoder.next() {
                switch packet {
                case .event(.channelReady): events += 1
                case .media(let frame, let data):
                    XCTAssertEqual(frame.resourceID, 2)
                    XCTAssertEqual(data, Data([1, 2, 3])); media += 1
                default: XCTFail("Unexpected packet")
                }
            }
        }
        try decoder.finish()
        XCTAssertEqual(events, 1); XCTAssertEqual(media, 1)
    }
    func testRejectsWrongHandshakeTruncationAndOversizedMedia() throws {
        var invalid = RemoteStreamDecoder()
        invalid.append(Data("login banner".utf8))
        XCTAssertThrowsError(try invalid.next())
        var duplicate = RemoteStreamDecoder()
        duplicate.append(try ready() + ready())
        _ = try duplicate.next()
        XCTAssertThrowsError(try duplicate.next())
        var short = RemoteStreamDecoder()
        short.append(Data([78]))
        XCTAssertThrowsError(try short.finish())
        var oversized = RemoteStreamDecoder()
        oversized.append(try ready())
        _ = try oversized.next()
        oversized.append(MediaWire.Header(surfaceID: 1, resourceID: 1,
            width: 1, height: 1, ptsNanos: 0, payloadLength: UInt32.max).encoded())
        XCTAssertThrowsError(try oversized.next())
    }
    func testOldCompositorReportsVersionMismatchBeforeConnecting() throws {
        // The installed remote compositor sent this protocol-7 handshake.
        let payload = Data(WindowWire.lifecycleMagic + [1, 1, 0, 0, 42, 140, 0, 0, 7, 0, 0, 0])
        var decoder = RemoteStreamDecoder()
        decoder.append(try WireFormat.frame(payload: payload))
        XCTAssertThrowsError(try decoder.next()) { error in
            XCTAssertEqual(error as? WindowWire.DecodeError, .unsupportedWindowVersion(7))
            XCTAssertTrue(error.localizedDescription.contains("Update NativePipe"))
        }
    }
    func testShellQuotingAndNoForwarding() throws {
        let command = SSHCommand(destination: "user@host", application: ["echo", "a'b", "$(touch /tmp/no)"])
        let arguments = try command.arguments()
        XCTAssertTrue(arguments.contains("ClearAllForwardings=yes"))
        XCTAssertFalse(arguments.contains("-L"))
        XCTAssertFalse(arguments.contains("-R"))
        XCTAssertTrue(command.remoteScript.contains("exec"))
        XCTAssertFalse(command.remoteScript.contains("nohup"))
        let process = Process()
        let output = Pipe()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        process.arguments = ["-c", "printf '%s' " + SSHCommand.quote("a'b $(no)")]
        process.standardOutput = output
        try process.run()
        let bytes = output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        XCTAssertEqual(String(decoding: bytes, as: UTF8.self), "a'b $(no)")
        XCTAssertThrowsError(try SSHCommand(destination: "-oProxyCommand=evil", application: ["true"]).validate())
    }
    func testOnlyPasswordsAndPassphrasesMayBeRemembered() {
        XCTAssertTrue(SSHCredentialStore.mayRemember("user@host's password: "))
        XCTAssertTrue(SSHCredentialStore.mayRemember("Enter passphrase for key '/a/b':"))
        XCTAssertFalse(SSHCredentialStore.mayRemember("Verification code:"))
        XCTAssertFalse(SSHCredentialStore.mayRemember("Are you sure you want to continue connecting?"))
    }
    @MainActor func testRealPipeEOFAndCancellation() async throws {
        let encoded = try ready().base64EncodedString()
        let session = RemoteSession(testExecutable: "/bin/sh", arguments: [
            "-c", "printf '%s' '\(encoded)' | /usr/bin/base64 -D; sleep 0.2"
        ])
        let ended = expectation(description: "EOF")
        session.onStateChange = { state in if state == .disconnected { ended.fulfill() } }
        try await session.connect()
        await fulfillment(of: [ended], timeout: 3)

        let idle = RemoteSession(testExecutable: "/bin/sh", arguments: [
            "-c", "printf '%s' '\(encoded)' | /usr/bin/base64 -D; read reply"
        ])
        let readyTask = Task { try await idle.connect() }
        let start = Date()
        let deadline = Task { try? await Task.sleep(for: .seconds(2)); readyTask.cancel() }
        do { try await readyTask.value } catch { XCTFail("Handshake waited for EOF: \(error)") }
        deadline.cancel()
        XCTAssertLessThan(Date().timeIntervalSince(start), 1)
        idle.disconnect()

        let waiting = RemoteSession(testExecutable: "/bin/sleep", arguments: ["5"])
        let task = Task { try await waiting.connect() }
        try await Task.sleep(nanoseconds: 30_000_000)
        task.cancel()
        do { try await task.value; XCTFail("Cancellation succeeded") }
        catch is CancellationError { }
        waiting.disconnect()
    }
}
