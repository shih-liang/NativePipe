import Foundation
import NativePipeProtocol
import Darwin

/// One SSH child per application invocation. SSH owns authentication, host-key
/// checks, encryption and transport; it never creates a forwarding listener.
@MainActor
public final class RemoteSession {
    public enum State: Sendable, Equatable { case disconnected, connected }
    public var onEvent: ((Windowing.GuestEvent) -> Void)?
    public var onMediaFrame: ((MediaWire.Header, Data) -> Void)?
    public var onStateChange: ((State) -> Void)?
    public var onDiagnostic: ((String) -> Void)?
    public var onError: ((Error) -> Void)?
    public private(set) var exitStatus: Int32?
    public lazy var applicationClient = ApplicationClient { [weak self] in self?.send($0) }
    private let command: SSHCommand
    private let environment: [String: String]?
    private let executable: String
    private let argumentsOverride: [String]?
    private var process: Process?
    private var generation = 0
    private var continuation: CheckedContinuation<Void, Error>?
    private var isReady = false
    public var isConnected: Bool { isReady }
    private var diagnostics = ""
    private var authenticationDirectory: URL?
    private let writer = WindowCommandWriter(write: RemoteSession.writeAll)

    public init(command: SSHCommand, environment: [String: String]? = nil) {
        self.command = command
        self.environment = environment
        executable = "/usr/bin/ssh"
        argumentsOverride = nil
    }

    // Uses real pipes and the same lifecycle in transport tests.
    init(testExecutable: String, arguments: [String]) {
        command = SSHCommand(destination: "test", application: ["true"])
        environment = nil
        executable = testExecutable
        argumentsOverride = arguments
    }

    public func connect() async throws {
        disconnect()
        generation += 1
        let token = generation
        diagnostics = ""
        exitStatus = nil
        let child = Process()
        child.executableURL = URL(fileURLWithPath: executable)
        child.arguments = try argumentsOverride ?? command.arguments()
        var childEnvironment = environment ?? SSHAuthentication.environment(
            askpassExecutable: URL(fileURLWithPath: CommandLine.arguments[0]))
        childEnvironment["NATIVEPIPE_SSH_CONNECTION"] = command.credentialID
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
                    // Close parent copies of child ends so EOF is observable.
                    try input.fileHandleForReading.close()
                    try output.fileHandleForWriting.close()
                    try errors.fileHandleForWriting.close()
                    let fd = input.fileHandleForWriting.fileDescriptor
                    guard fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0 else {
                        throw POSIXError(.EIO)
                    }
                    writer.install(input.fileHandleForWriting)
                    Task.detached { [weak self] in
                        var decoder = RemoteStreamDecoder()
                        do {
                            while true {
                                let bytes = try Self.readChunk(output.fileHandleForReading, capacity: 65_536)
                                if bytes.isEmpty { break }
                                decoder.append(bytes)
                                while let packet = try decoder.next() {
                                    await self?.receive(packet, token: token)
                                }
                            }
                            try decoder.finish()
                            child.waitUntilExit()
                            await self?.ended(child.terminationStatus, token: token)
                        } catch {
                            await self?.fail(error, token: token)
                        }
                        try? output.fileHandleForReading.close()
                    }
                    Task.detached { [weak self] in
                        while let bytes = try? Self.readChunk(errors.fileHandleForReading, capacity: 4096),
                              !bytes.isEmpty {
                            await self?.diagnostic(String(decoding: bytes, as: UTF8.self), token: token)
                        }
                        try? errors.fileHandleForReading.close()
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
        writer.send(command)
    }
    public func applications(refresh: Bool = false) async throws -> [GuestApplication] {
        try await applicationClient.applications(refresh: refresh)
    }
    public func launchApplication(_ id: String) async throws {
        _ = try await applicationClient.launch(id)
    }

    public func disconnect() {
        generation += 1
        continuation?.resume(throwing: CancellationError())
        continuation = nil
        writer.disconnect()
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
                isReady = true
                applicationClient.setConnected(true)
                continuation?.resume()
                continuation = nil
                onStateChange?(.connected)
            }
            onEvent?(event)
        case .media(let header, let bytes):
            onMediaFrame?(header, bytes)
        case .applications(let reply):
            applicationClient.receive(reply)
        }
    }

    private func diagnostic(_ text: String, token: Int) {
        guard token == generation else { return }
        diagnostics = String((diagnostics + text).suffix(8192))
        onDiagnostic?(text)
    }

    private func ended(_ status: Int32, token: Int) {
        guard token == generation else { return }
        exitStatus = status
        if status != 0 || !isReady {
            fail(RemoteError.message(diagnostics.isEmpty
                ? "Remote command exited with status \(status)." : diagnostics), token: token)
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
