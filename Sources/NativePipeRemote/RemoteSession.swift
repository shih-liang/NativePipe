import NativePipeStrings
import Foundation
import NativePipeProtocol
import Darwin

/// One SSH child per application invocation. SSH owns authentication, host-key
/// checks, encryption and transport; it never creates a forwarding listener.
@MainActor
public final class RemoteSession {
    public enum State: Sendable, Equatable { case disconnected, connected }
    public var onEvent: ((Windowing.GuestEvent) -> Void)?
    /// Called on the connection reader, never AppKit's main actor.
    public var onMediaFrame: (@Sendable (MediaWire.Header, Data) -> Bool)?
    public var onStateChange: ((State) -> Void)?
    public var onDiagnostic: ((String) -> Void)?
    public var onError: ((Error) -> Void)?
    public private(set) var exitStatus: Int32?
    /// The remote command's own exit status, set only when it actually exited.
    /// `exitStatus` is also synthesized for local failures, so it cannot tell a
    /// command that exited with 1 apart from a stream this Mac rejected.
    public private(set) var remoteExitStatus: Int32?
    /// SSH and remote stderr as a person should read it: NativePipe's own
    /// startup markers removed, capped like the raw buffer.
    public private(set) var visibleDiagnostics = ""
    private var diagnosticLines = DiagnosticLines()
    public lazy var applicationClient = ApplicationClient { [weak self] in self?.send($0) }
    private let command: SSHCommand
    private let environment: [String: String]?
    private let executable: String
    private let argumentsOverride: [String]?
    private let localCompositorDirectory: URL?
    private let allowHardwareH264: Bool
    private var process: Process?
    private var generation = 0
    private var continuation: CheckedContinuation<Void, Error>?
    private var isReady = false
    public var isConnected: Bool { isReady }
    private var diagnostics = ""
    private var authenticationDirectory: URL?
    private var writer: WindowCommandWriter?
    private var inbound: RemoteInbound?
    public enum StartupPhase: Sendable { case connecting, authenticating, installing, ready, connected }
    public var onStartupPhaseChange: ((StartupPhase) -> Void)?
    public private(set) var startupPhase = StartupPhase.connecting {
        didSet { if oldValue != startupPhase { onStartupPhaseChange?(startupPhase) } }
    }
    private var startupTimeout: Task<Void, Never>?
    private var readyTimeout: Duration = .seconds(15)
    private var reportsStartup = true

    public init(command: SSHCommand, environment: [String: String]? = nil,
                localCompositorDirectory: URL? = nil, allowHardwareH264: Bool = true) {
        self.command = command
        self.environment = environment
        self.localCompositorDirectory = localCompositorDirectory
        self.allowHardwareH264 = allowHardwareH264
        executable = "/usr/bin/ssh"
        argumentsOverride = nil
    }

    // Uses real pipes and the same lifecycle in transport tests.
    init(testExecutable: String, arguments: [String], localCompositorDirectory: URL? = nil,
         readyTimeout: Duration = .seconds(15), reportsStartup: Bool = false) {
        command = SSHCommand(destination: "test", application: ["true"], installCompositor: localCompositorDirectory != nil)
        environment = nil
        allowHardwareH264 = false
        self.localCompositorDirectory = localCompositorDirectory
        executable = testExecutable
        argumentsOverride = arguments
        self.readyTimeout = readyTimeout
        self.reportsStartup = reportsStartup
    }

    public func connect() async throws {
        disconnect()
        generation += 1
        let token = generation
        diagnostics = ""
        visibleDiagnostics = ""
        diagnosticLines = DiagnosticLines()
        remoteExitStatus = nil
        startupPhase = .connecting
        onStartupPhaseChange?(.connecting)
        exitStatus = nil
        do { try await establishConnection(token: token) }
        catch {
            if generation == token {
                if !Task.isCancelled && !(error is CancellationError) { exitStatus = 1 }
                disconnect()
            }
            throw error
        }
    }

    private func establishConnection(token: Int) async throws {
        let localCompositor = command.installCompositor && command.compositor == "nativepipe-wayland"
            ? localCompositorDirectory : nil
        let arguments: [String]
        if let argumentsOverride { arguments = argumentsOverride }
        else { arguments = try await command.arguments(uploadCompositor: localCompositor != nil,
            hardwareH264: allowHardwareH264 && H264Decoder.hardwareAvailable, reportStartup: true) }
        try Task.checkCancellation()
        guard generation == token else { throw CancellationError() }
        let writer = WindowCommandWriter(remote: true, write: Self.writeAll)
        self.writer = writer
        let inbound = RemoteInbound(writer: writer, media: onMediaFrame) { [weak self] event in
            guard let self, self.generation == token else { return }
            switch event {
            case .packet(let packet): self.receive(packet, token: token)
            case .diagnostic(let text): self.diagnostic(text, token: token)
            case .ended(let status): self.ended(status, token: token)
            case .failed(let error): self.fail(error, token: token)
            }
        }
        self.inbound = inbound
        let child = Process()
        child.executableURL = URL(fileURLWithPath: executable)
        var childEnvironment = environment ?? SSHAuthentication.environment(
            askpassExecutable: URL(fileURLWithPath: CommandLine.arguments[0]))
        childEnvironment["NATIVEPIPE_SSH_CONNECTION"] = command.credentialID
        child.arguments = (argumentsOverride == nil
            ? SSHAuthentication.preferredAuthenticationArguments(connection: command.credentialID, environment: childEnvironment,
                sshArguments: command.sshArguments) : []) + arguments
        let authDirectory = try SSHCredentialStore.makeAttemptDirectory(environment: childEnvironment)
        authenticationDirectory = authDirectory
        childEnvironment["NATIVEPIPE_SSH_AUTH_SESSION"] = authDirectory.path
        child.environment = childEnvironment
        let input = Pipe(), output = Pipe(), errors = Pipe()
        child.standardInput = input
        child.standardOutput = output
        child.standardError = errors
        process = child
        writer.onFailure = { [weak self] error in
            // The SSH child can close stdin just before returning its exit
            // status. Its output EOF owns normal shutdown and the exit code.
            guard (error as? POSIXError)?.code != .EPIPE else { return }
            Task { @MainActor in self?.fail(error, token: token) }
        }
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { ready in
                continuation = ready
                do {
                    try child.run()
                    if !reportsStartup { advanceStartup(to: .ready, token: token) }
                    // Close parent copies of child ends so EOF is observable.
                    try input.fileHandleForReading.close()
                    try output.fileHandleForWriting.close()
                    try errors.fileHandleForWriting.close()
                    let fd = input.fileHandleForWriting.fileDescriptor
                    guard fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0 else {
                        throw POSIXError(.EIO)
                    }
                    writer.install(input.fileHandleForWriting)
                    // The bootstrap owns a duplicate until it hands stdin to
                    // the protocol writer. Disconnect can close the writer's
                    // descriptor without reusing a descriptor under this I/O.
                    let uploadInput: FileHandle?
                    if localCompositor != nil {
                        let descriptor = fcntl(fd, F_DUPFD_CLOEXEC, 0)
                        guard descriptor >= 0 else { throw POSIXError(.EMFILE) }
                        uploadInput = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
                    } else { uploadInput = nil }
                    let diagnosticsTask = Task.detached {
                        while let bytes = try? Self.readChunk(errors.fileHandleForReading, capacity: 4096),
                              !bytes.isEmpty {
                            inbound.enqueue(.diagnostic(String(decoding: bytes, as: UTF8.self)), bytes: bytes.count)
                        }
                        try? errors.fileHandleForReading.close()
                    }
                    Task.detached {
                        defer { try? uploadInput?.close() }
                        var decoder = RemoteStreamDecoder()
                        do {
                            var prepared = true
                            if let localCompositor, let uploadInput {
                                prepared = try RemoteCompositorUpload.prepare(directory: localCompositor,
                                    input: uploadInput, output: output.fileHandleForReading, write: Self.writeAll)
                                try uploadInput.close()
                            }
                            while prepared {
                                let bytes = try Self.readChunk(output.fileHandleForReading, capacity: 65_536)
                                if bytes.isEmpty { break }
                                decoder.append(bytes)
                                while let packet = try decoder.next() {
                                    guard inbound.receive(packet, bytes: decoder.packetBytes) else {
                                        try? output.fileHandleForReading.close()
                                        return
                                    }
                                }
                            }
                            try decoder.finish()
                            child.waitUntilExit()
                            // stderr and stdout are independent pipes. Deliver
                            // the final SSH/installer error before its exit can
                            // stop inbound delivery and discard that diagnostic.
                            await diagnosticsTask.value
                            inbound.enqueue(.ended(child.terminationStatus))
                        } catch {
                            inbound.enqueue(.failed(error))
                        }
                        try? output.fileHandleForReading.close()
                    }
                } catch { fail(error, token: token) }
            }
        } onCancel: {
            Task { @MainActor [weak self] in
                guard self?.generation == token else { return }
                self?.disconnect()
            }
        }
    }

    public func send(_ command: Windowing.HostCommand) {
        guard isReady else { return }
        writer?.send(command)
    }
    public func sceneCompleted(surface: UInt32, presentationID: UInt32, displayed: Bool, intervalNanoseconds: UInt32) {
        guard isReady else { return }
        writer?.remotePresentation(surface: surface, presentationID: presentationID, displayed: displayed, intervalNanoseconds: intervalNanoseconds)
    }
    public func applications(refresh: Bool = false) async throws -> [GuestApplication] {
        try await applicationClient.applications(refresh: refresh)
    }
    public func launchApplication(_ id: String) async throws {
        _ = try await applicationClient.launch(id)
    }

    public func disconnect() {
        startupTimeout?.cancel()
        startupTimeout = nil
        generation += 1
        continuation?.resume(throwing: CancellationError())
        continuation = nil
        inbound?.stop()
        inbound = nil
        writer?.disconnect()
        writer = nil
        applicationClient.setConnected(false)
        if process?.isRunning == true { process?.terminate() }
        process = nil
        if let directory = authenticationDirectory {
            try? FileManager.default.removeItem(at: directory)
            authenticationDirectory = nil
        }
        let wasReady = isReady
        isReady = false
        if wasReady { onStateChange?(.disconnected) }
    }

    private func receive(_ packet: RemoteStreamDecoder.Packet, token: Int) {
        guard token == generation else { return }
        switch packet {
        case .event(let event):
            if case .channelReady = event {
                startupTimeout?.cancel()
                startupTimeout = nil
                startupPhase = .connected
                isReady = true
                applicationClient.setConnected(true)
                continuation?.resume()
                continuation = nil
                onStateChange?(.connected)
            }
            onEvent?(event)
        case .media, .acknowledge, .credit:
            break // Handled by this connection's reader, before UI delivery.
        case .applications(let reply):
            applicationClient.receive(reply)
        }
    }

    private func diagnostic(_ text: String, token: Int) {
        guard token == generation else { return }
        diagnostics = String((diagnostics + text).suffix(65_536))
        if !isReady, diagnostics.contains(SSHAuthentication.cancelledDiagnostic) {
            // Stop before OpenSSH can offer another authentication method and
            // reopen a prompt the user has just cancelled.
            disconnect()
            return
        }
        // stderr reads can split a marker. Accumulated diagnostics preserve its
        // boundary; phase transitions are monotonic, so later logs cannot reset
        // the deadline. No timer runs while SSH asks the user to authenticate.
        if reportsStartup {
            if diagnostics.contains("NATIVEPIPE PHASE READY\n") { advanceStartup(to: .ready, token: token) }
            else if diagnostics.contains("NATIVEPIPE PHASE INSTALLING\n") {
                advanceStartup(to: .installing, token: token)
            } else if diagnostics.contains("NATIVEPIPE PHASE AUTHENTICATING\n"), startupPhase == .connecting {
                startupPhase = .authenticating
            }
        }
        show(diagnosticLines.visible(text))
    }

    private func show(_ text: String) {
        guard !text.isEmpty else { return }
        visibleDiagnostics = String((visibleDiagnostics + text).suffix(65_536))
        onDiagnostic?(text)
    }

    /// A final line without a newline is held back in case it is the first half
    /// of a marker; once the stream is over it can only be text.
    private func flushDiagnostics() {
        show(diagnosticLines.finish())
    }

    /// The diagnostics as an error message: no markers, no trailing newline.
    private var diagnosticMessage: String {
        visibleDiagnostics.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func advanceStartup(to phase: StartupPhase, token: Int) {
        guard generation == token, !isReady,
              startupPhase == .connecting || startupPhase == .authenticating || (startupPhase == .installing && phase == .ready) else { return }
        startupPhase = phase
        startupTimeout?.cancel()
        // Upload and runtime validation have a separate generous deadline.
        // Once exec is imminent, only the compositor handshake gets 15 seconds.
        let timeout: Duration = phase == .ready ? readyTimeout : .seconds(600)
        startupTimeout = Task { [weak self] in
            do { try await Task.sleep(for: timeout) } catch { return }
            guard let self, self.generation == token, !self.isReady else { return }
            let message = phase == .ready
                ? NPText("The NativePipe compositor didn’t start in time. The connection messages may show why.")
                : NPText("NativePipe installation did not finish in time. Check the remote connection and available disk space.")
            self.flushDiagnostics()
            let details = self.diagnosticMessage
            self.fail(RemoteError.message(message + (details.isEmpty ? "" : "\n" + details)), token: token)
        }
    }

    private func ended(_ status: Int32, token: Int) {
        guard token == generation else { return }
        flushDiagnostics()
        exitStatus = status
        remoteExitStatus = status
        if status != 0 || !isReady {
            fail(RemoteError.message(diagnosticMessage.isEmpty
                ? NPText("Remote command exited with status %@.", String(status)) : diagnosticMessage), token: token)
        } else { disconnect() }
    }

    private func fail(_ error: Error, token: Int) {
        guard token == generation else { return }
        if exitStatus == nil { exitStatus = 1 }
        let connecting = continuation != nil
        continuation?.resume(throwing: error)
        continuation = nil
        if !connecting { onError?(error) }
        disconnect()
    }

    func decodingFailed(_ message: String) {
        fail(RemoteError.message(message), token: generation)
    }

    nonisolated private static func writeAll(_ handle: FileHandle, _ data: Data) throws {
        // macOS supports per-descriptor SIGPIPE suppression for pipes too.
        guard fcntl(handle.fileDescriptor, F_SETNOSIGPIPE, 1) == 0 else { throw POSIXError(.EIO) }
        try data.withUnsafeBytes { bytes in
            var offset = 0
            let deadline = ProcessInfo.processInfo.systemUptime + 10
            while offset < bytes.count {
                let count = Darwin.write(handle.fileDescriptor, bytes.baseAddress! + offset, bytes.count - offset)
                if count > 0 { offset += count; continue }
                if count < 0 && errno == EINTR { continue }
                if count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
                   ProcessInfo.processInfo.systemUptime < deadline {
                    var fd = pollfd(fd: handle.fileDescriptor, events: Int16(POLLOUT), revents: 0)
                    _ = Darwin.poll(&fd, 1, 50)
                    continue
                }
                let error = errno
                throw POSIXError(POSIXErrorCode(rawValue: error) ?? .EIO)
            }
        }
    }

    // FileHandle.read(upToCount:) waits to fill its requested count on macOS
    // pipes. A handshake/small input reply must be delivered before EOF, so use
    // one POSIX read and let the incremental decoder retain partial packets.
    nonisolated private static func readChunk(_ handle: FileHandle, capacity: Int) throws -> Data {
        var buffer = [UInt8](repeating: 0, count: capacity)
        while true {
            let count = Darwin.read(handle.fileDescriptor, &buffer, capacity)
            if count >= 0 { return Data(buffer.prefix(count)) }
            if errno != EINTR { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        }
    }
}

/// Splits SSH's stderr into the text a person should read and NativePipe's own
/// startup markers ("NATIVEPIPE PHASE READY" and the like). The markers drive
/// startup tracking; they are protocol, and echoing them to a terminal or into
/// an error dialog shows the user a packet header.
///
/// Works a line at a time, because a read can end halfway through a marker.
/// Scans Unicode scalars rather than Characters: Swift treats "\r\n" as one
/// Character, so searching Characters for "\n" would miss CRLF line ends.
struct DiagnosticLines {
    private var pending = String.UnicodeScalarView()

    /// The complete, non-marker lines in `text`, newlines included. A trailing
    /// partial line is held until the rest of it arrives.
    mutating func visible(_ text: String) -> String {
        pending.append(contentsOf: text.unicodeScalars)
        guard let lastNewline = pending.lastIndex(of: "\n") else { return "" }
        let complete = pending[...lastNewline]
        pending = String.UnicodeScalarView(pending[pending.index(after: lastNewline)...])
        var result = String.UnicodeScalarView()
        var start = complete.startIndex
        for index in complete.indices where complete[index] == "\n" {
            let line = complete[start...index]
            if !Self.isMarker(String(String.UnicodeScalarView(line))) {
                result.append(contentsOf: line)
            }
            start = complete.index(after: index)
        }
        return String(result)
    }

    /// The held partial line, once the stream has ended.
    mutating func finish() -> String {
        defer { pending = String.UnicodeScalarView() }
        let rest = String(pending)
        return Self.isMarker(rest) ? "" : rest
    }

    /// "NATIVEPIPE" followed only by upper-case words. Ordinary messages that
    /// mention the product spell it "NativePipe", so they never match.
    static func isMarker(_ line: String) -> Bool {
        let body = line.trimmingCharacters(in: .newlines)
        guard body.hasPrefix("NATIVEPIPE ") else { return false }
        return body.unicodeScalars.dropFirst(11).allSatisfy {
            (65...90).contains($0.value) || (48...57).contains($0.value) || $0 == " " || $0 == "_"
        }
    }
}
