import AppKit
import XCTest
import NativePipeStrings
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
        let stream = try ready() + RemoteWire.fragment(header.encoded() + Data([1, 2, 3]), offset: 0, lane: 1)
        var decoder = RemoteStreamDecoder(), events = 0, media = 0
        for byte in stream {
            decoder.append(Data([byte]))
            while let packet = try decoder.next() {
                switch packet {
                case .event(.channelReady): events += 1
                case .media(let frame, let data):
                    XCTAssertEqual(frame.resourceID, 2)
                    XCTAssertEqual(data, Data([1, 2, 3])); media += 1
                case .acknowledge(let count): XCTAssertEqual(count, MediaWire.headerSize + 3)
                default: XCTFail("Unexpected packet")
                }
            }
        }
        try decoder.finish()
        XCTAssertEqual(events, 1); XCTAssertEqual(media, 1)
    }
    func testControlBypassesIncompleteBulkRecord() throws {
        let header = MediaWire.Header(surfaceID: 1, resourceID: 2,
            width: 4, height: 4, ptsNanos: 0, payloadLength: 50_000)
        let record = header.encoded() + Data(repeating: 42, count: 50_000)
        var decoder = RemoteStreamDecoder()
        decoder.append(try ready())
        _ = try decoder.next()
        decoder.append(RemoteWire.fragment(record, offset: 0, lane: 1))
        guard case .acknowledge(RemoteWire.fragmentSize) = try decoder.next() else { return XCTFail("Missing credit") }
        XCTAssertNil(try decoder.next())
        // Credit and interaction replies must be visible while a large video
        // record is still incomplete, without corrupting that record.
        decoder.append(try WireFormat.frame(payload: RemoteWire.acknowledgement(123)))
        guard case .credit(123) = try decoder.next() else { return XCTFail("Control blocked by video") }
        for offset in stride(from: RemoteWire.fragmentSize, to: record.count, by: RemoteWire.fragmentSize) {
            decoder.append(RemoteWire.fragment(record, offset: offset, lane: 1))
            guard case .acknowledge = try decoder.next() else { return XCTFail("Missing credit") }
        }
        guard case .media(_, let bytes) = try decoder.next() else { return XCTFail("Missing completed record") }
        XCTAssertEqual(bytes, Data(repeating: 42, count: 50_000))
        try decoder.finish()
        var fragments = RemoteWire.Reassembler()
        let packet = RemoteWire.fragment(record, offset: RemoteWire.fragmentSize, lane: 1)
        XCTAssertThrowsError(try fragments.receive(Data(packet.dropFirst(WireFormat.headerSize)), maximumSize: 60_000))
    }
    func testMalformedOpenRequestPreservesFollowingWindowAndMediaRecords() throws {
        // A complete optional request has a token but an invalid nested frame.
        let malformed = Data(WindowWire.lifecycleMagic + [1, 40, 0, 0,
            9, 0, 0, 0, 1, 0, 0, 0, 0])
        let created = Data(WindowWire.lifecycleMagic + [1, 2, 0, 0, 17, 0, 0, 0])
        let header = MediaWire.Header(surfaceID: 17, resourceID: 2,
            width: 1, height: 1, ptsNanos: 0, payloadLength: 3)
        var decoder = RemoteStreamDecoder()
        decoder.append(try ready() + WireFormat.frame(payload: malformed)
            + WireFormat.frame(payload: created)
            + RemoteWire.fragment(header.encoded() + Data([1, 2, 3]), offset: 0, lane: 1))
        guard case .event(.channelReady) = try decoder.next() else { return XCTFail("Missing handshake") }
        guard case .event(.hostOpenRejected(token: 9)) = try decoder.next() else { return XCTFail("Missing optional refusal") }
        guard case .event(.surfaceCreated(surface: 17)) = try decoder.next() else { return XCTFail("Window record was lost") }
        guard case .acknowledge = try decoder.next() else { return XCTFail("Missing credit") }
        guard case .media(_, let bytes) = try decoder.next() else { return XCTFail("Display was disconnected") }
        XCTAssertEqual(bytes, Data([1, 2, 3]))
        XCTAssertNil(try decoder.next())
        try decoder.finish()
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
    /// OpenSSH keeps the first value it sees for a -o option, so the session's
    /// safety settings hold only while they precede everything the user passed.
    /// A user's -o RequestTTY=force or -o ControlPath=... must lose to them.
    func testBuiltInSafetyOptionsPrecedeUserArguments() async throws {
        let user = ["-A", "-o", "RequestTTY=force", "-o", "ControlPath=/tmp/shared"]
        let command = SSHCommand(destination: "user@host", application: ["app"], sshArguments: user)
        let arguments = try await command.arguments()
        let firstUser = try XCTUnwrap(arguments.firstIndex(of: "-A"))
        for safety in ["-T", "ControlPath=none", "ClearAllForwardings=yes"] {
            let index = try XCTUnwrap(arguments.firstIndex(of: safety), safety)
            XCTAssertLessThan(index, firstUser, safety)
        }
        XCTAssertFalse(arguments.contains("-t"))
    }
    /// AppKit text controls -- the SSH password prompt above all -- get
    /// Cut/Copy/Paste/Select All only from these key equivalents.
    @MainActor
    func testStandardEditMenuCarriesTheTextKeyEquivalents() throws {
        let menu = try XCTUnwrap(StandardEditMenu.item().submenu)
        func item(_ key: String, _ modifiers: NSEvent.ModifierFlags = .command) -> NSMenuItem? {
            menu.items.first { $0.keyEquivalent == key && $0.keyEquivalentModifierMask == modifiers }
        }
        XCTAssertEqual(item("x")?.action, #selector(NSText.cut(_:)))
        XCTAssertEqual(item("c")?.action, #selector(NSText.copy(_:)))
        XCTAssertEqual(item("v")?.action, #selector(NSText.paste(_:)))
        XCTAssertEqual(item("a")?.action, #selector(NSText.selectAll(_:)))
        XCTAssertEqual(item("z")?.action, Selector(("undo:")))
        XCTAssertEqual(item("z", [.command, .shift])?.action, Selector(("redo:")))
        // Nil targets, so AppKit validates them against the responder chain.
        XCTAssertTrue(menu.items.allSatisfy { $0.target == nil })
    }

    func testShellQuotingAndNoForwarding() async throws {
        let command = SSHCommand(destination: "user@host", application: ["echo", "a'b", "$(touch /tmp/no)"])
        let arguments = try await command.arguments()
        XCTAssertTrue(arguments.contains("ClearAllForwardings=yes"))
        XCTAssertTrue(arguments.contains("-C"))
        XCTAssertTrue(arguments.contains("ControlPath=none"))
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

    @MainActor func testFailureDrainsFinalDiagnosticAndAllowsImmediateReconnect() async throws {
        // Repeated immediate exits expose the race between stdout EOF and the
        // last stderr read, without introducing a timing sleep into the child.
        let message = "SSH authentication failed: final diagnostic"
        let session = RemoteSession(testExecutable: "/bin/sh", arguments: [
            "-c", "printf '%s' " + SSHCommand.quote(message) + " >&2; exit 255"
        ])
        for _ in 0..<20 {
            do { try await session.connect(); XCTFail("Expected failure") }
            catch { XCTAssertEqual(error.localizedDescription, message) }
            XCTAssertFalse(session.isConnected)
            XCTAssertEqual(session.exitStatus, 255)
        }
    }

    @MainActor func testCompositorReadyDeadlineDoesNotRequireManualDisconnect() async throws {
        let session = RemoteSession(testExecutable: "/bin/sleep", arguments: ["10"],
                                    readyTimeout: .milliseconds(50))
        for _ in 0..<2 {
            let start = ContinuousClock.now
            do { try await session.connect(); XCTFail("Expected automatic timeout") }
            catch {
                XCTAssertTrue(error.localizedDescription.hasPrefix(
                    NPText("The NativePipe compositor didn’t start in time. The connection messages may show why.")))
            }
            XCTAssertLessThan(start.duration(to: .now), .seconds(2))
            XCTAssertFalse(session.isConnected)
            XCTAssertEqual(session.exitStatus, 1)
        }
    }

    @MainActor func testStartupDeadlineBeginsAfterInstallationAndEndsAtReady() async throws {
        let encoded = try ready().base64EncodedString()
        let session = RemoteSession(testExecutable: "/bin/sh", arguments: ["-c", """
            printf 'NATIVEPIPE PHASE INSTALLING\\n' >&2
            sleep 0.15
            printf 'NATIVEPIPE PHASE RE' >&2
            sleep 0.05
            printf 'ADY\\n' >&2
            printf '%s' '\(encoded)' | /usr/bin/base64 -D
            read reply
            """], readyTimeout: .milliseconds(100), reportsStartup: true)
        try await session.connect()
        try await Task.sleep(for: .milliseconds(150))
        XCTAssertTrue(session.isConnected)
        session.disconnect()
    }

    @MainActor func testObservedStartupPhasesSurviveSplitMarkersAndReconnect() async throws {
        let encoded = try ready().base64EncodedString()
        let session = RemoteSession(testExecutable: "/bin/sh", arguments: ["-c", """
            printf 'NATIVEPIPE PHASE AUTHENTI' >&2
            sleep 0.03
            printf 'CATING\\n' >&2
            sleep 0.03
            printf 'NATIVEPIPE PHASE INSTALLING\\n' >&2
            sleep 0.03
            printf 'NATIVEPIPE PHASE READY\\n' >&2
            sleep 0.03
            printf '%s' '\(encoded)' | /usr/bin/base64 -D
            read reply
            """], reportsStartup: true)
        for _ in 0..<2 {
            var phases: [RemoteSession.StartupPhase] = []
            session.onStartupPhaseChange = { phases.append($0) }
            try await session.connect()
            XCTAssertEqual(Array(phases.suffix(4)), [.authenticating, .installing, .ready, .connected])
            XCTAssertEqual(phases.first, .connecting)
            session.disconnect()
            XCTAssertFalse(session.isConnected)
        }
    }

    @MainActor func testInstallationTimesOutOnlyWhenProgressStops() async throws {
        let encoded = try ready().base64EncodedString()
        let active = RemoteSession(testExecutable: "/bin/sh", arguments: ["-c", """
            printf 'NATIVEPIPE PHASE INSTALLING\\n' >&2
            for i in 1 2 3 4 5 6; do sleep 0.06; printf 'Installing…\\n' >&2; done
            printf 'NATIVEPIPE PHASE READY\\n' >&2
            printf '%s' '\(encoded)' | /usr/bin/base64 -D
            read reply
            """], reportsStartup: true, installationTimeout: .milliseconds(200))
        defer { active.disconnect() }
        try await active.connect()
        XCTAssertTrue(active.isConnected, "Progressing installation may last longer than the idle timeout")
        let stalled = RemoteSession(testExecutable: "/bin/sh", arguments: ["-c",
            "printf 'NATIVEPIPE PHASE INSTALLING\\n' >&2; sleep 5"], reportsStartup: true,
            installationTimeout: .milliseconds(200))
        defer { stalled.disconnect() }
        do { try await stalled.connect(); XCTFail("A stalled installer must fail") }
        catch { XCTAssertTrue(error.localizedDescription.contains("installation did not finish in time")) }
    }

    @MainActor func testFailureStageIsRealAndRetryDoesNotNeedManualDisconnect() async throws {
        for (marker, phase, reason) in [
            ("", RemoteSession.StartupPhase.connecting, "ssh: connect to host fixture.invalid: Connection refused"),
            ("NATIVEPIPE PHASE AUTHENTICATING\n", .authenticating, "Permission denied (publickey,password)."),
            ("NATIVEPIPE PHASE INSTALLING\n", .installing, "NativePipe installation failed: No space left on device")
        ] {
            let session = RemoteSession(testExecutable: "/bin/sh", arguments: ["-c",
                "printf '%s' " + SSHCommand.quote(marker + reason) + " >&2; exit 255"], reportsStartup: true)
            for _ in 0..<2 {
                do { try await session.connect(); XCTFail("Expected failure") }
                catch { XCTAssertTrue(error.localizedDescription.contains(reason)) }
                XCTAssertEqual(session.startupPhase, phase)
                XCTAssertFalse(session.isConnected)
            }
        }
    }

    @MainActor func testDismissingAuthenticationCancelsRatherThanFailingConnection() async throws {
        let session = RemoteSession(testExecutable: "/bin/sh", arguments: ["-c",
            "printf '%s' " + SSHCommand.quote(SSHAuthentication.cancelledDiagnostic) + " >&2; exit 255"], reportsStartup: true)
        for _ in 0..<2 {
            do { try await session.connect(); XCTFail("Expected cancellation") }
            catch is CancellationError { }
            XCTAssertFalse(session.isConnected)
        }
    }

    // MARK: What reaches the user from stderr

    /// NativePipe's own startup markers drive phase tracking. Printed, they are
    /// a packet header in the user's terminal or error dialog.
    func testDiagnosticLinesDropMarkersEvenWhenReadsSplitThem() {
        var lines = DiagnosticLines()
        var shown = lines.visible("Checking NativePipe v1…\nNATIVEPIPE PHASE INST")
        shown += lines.visible("ALLING\nssh: warning\r\nNATIVEPIPE PHASE READY\r\n")
        shown += lines.visible("NATIVEPIPE AUTH CANCELLED\nPermission denied")   // no final newline
        shown += lines.finish()
        XCTAssertEqual(shown, "Checking NativePipe v1…\nssh: warning\r\nPermission denied")
    }

    func testOnlyUpperCaseNativePipeLinesAreMarkers() {
        XCTAssertTrue(DiagnosticLines.isMarker("NATIVEPIPE PHASE READY\n"))
        XCTAssertTrue(DiagnosticLines.isMarker("NATIVEPIPE AUTH CANCELLED"))
        XCTAssertFalse(DiagnosticLines.isMarker("NativePipe compositor is up to date.\n"))
        XCTAssertFalse(DiagnosticLines.isMarker("NATIVEPIPE failed: see log\n"))
        var lines = DiagnosticLines()
        XCTAssertEqual(lines.visible("NATIVEPIPE PHA"), "")
        XCTAssertEqual(lines.finish(), "", "a marker cut off by the end of the stream is still a marker")
    }

    /// End to end through a real process: the reason survives (even without a
    /// trailing newline), the markers never reach onDiagnostic, the error text
    /// or visibleDiagnostics, and the remote status is kept apart from a local one.
    @MainActor func testMarkersNeverReachTheUserButTheReasonDoes() async throws {
        let session = RemoteSession(testExecutable: "/bin/sh", arguments: ["-c",
            "printf 'NATIVEPIPE PHASE AUTHENTI' >&2; printf 'CATING\\nPermission denied (publickey).' >&2; exit 255"],
            reportsStartup: true)
        var forwarded = ""
        session.onDiagnostic = { forwarded += $0 }
        do { try await session.connect(); XCTFail("Expected failure") }
        catch {
            XCTAssertEqual(error.localizedDescription, "Permission denied (publickey).")
        }
        XCTAssertEqual(session.startupPhase, .authenticating, "phase tracking still sees the marker")
        XCTAssertEqual(forwarded, "Permission denied (publickey).")
        XCTAssertEqual(session.visibleDiagnostics, "Permission denied (publickey).")
        XCTAssertEqual(session.remoteExitStatus, 255)
    }
}
