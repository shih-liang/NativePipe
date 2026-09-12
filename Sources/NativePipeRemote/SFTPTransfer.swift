import Foundation
import Darwin

/// Uses the installed OpenSSH client; files never pass through the display stream.
@MainActor
public final class SFTPTransfer {
    public enum Direction: String, Identifiable, Sendable {
        case upload, download
        public var id: String { rawValue }
    }
    private var process: Process?
    private var cancelled = false
    public init() {}

    public func cancel() {
        cancelled = true
        if let process, process.isRunning { process.terminate() }
    }

    public static func batch(direction: Direction, local: String, remote: String,
                             recursive: Bool = false, createParent: Bool = false) throws -> String {
        func quote(_ path: String) throws -> String {
            guard !path.isEmpty, !path.contains(where: { $0 == "\0" || $0 == "\n" || $0 == "\r" }) else {
                throw RemoteError.message("Enter a file path without line breaks.")
            }
            // SFTP protects glob characters inside quotes itself.
            var result = "\""
            for character in path {
                if "\\\"".contains(character) { result.append("\\") }
                result.append(character)
            }
            return result + "\""
        }
        let prefix: String
        if createParent {
            let parent = try quote((remote as NSString).deletingLastPathComponent)
            prefix = "mkdir \(parent)\nchmod 700 \(parent)\n"
        } else { prefix = "" }
        let local = try quote(local), remote = try quote(remote)
        let flags = recursive ? "-R " : ""
        return prefix + (direction == .upload ? "put \(flags)-- \(local) \(remote)\nbye\n"
                                    : "get \(flags)-- \(remote) \(local)\nbye\n")
    }

    public func run(command: SSHCommand, direction: Direction, local: URL, remote: String,
                    environment: [String: String], recursive: Bool = false, createParent: Bool = false) async throws {
        guard process == nil else { throw RemoteError.message("A file transfer is already running.") }
        try command.validate()
        let batch = try Self.batch(direction: direction, local: local.path, remote: remote,
                                  recursive: recursive, createParent: createParent)
        let child = Process(), input = Pipe(), output = Pipe()
        child.executableURL = URL(fileURLWithPath: "/usr/bin/sftp")
        var arguments = command.sshArguments
        // SSH and SFTP use different spellings for their port option.
        if let index = arguments.firstIndex(of: "-p") { arguments[index] = "-P" }
        var destination = command.destination
        let address = destination.split(separator: "@", omittingEmptySubsequences: false).last.map(String.init) ?? destination
        if address.contains(":"), !address.hasPrefix("[") {
            destination = String(destination.dropLast(address.count)) + "[" + address + "]"
        }
        child.arguments = ["-C", "-b", "-", "-o", "ControlPath=none", "-o", "BatchMode=no", "-o", "ClearAllForwardings=yes",
                           "-o", "ConnectTimeout=15", "-o", "ServerAliveInterval=30",
                           "-o", "ServerAliveCountMax=3"] + arguments + ["--", destination]
        let auth = try SSHCredentialStore.makeAttemptDirectory(environment: environment)
        defer { try? FileManager.default.removeItem(at: auth) }
        var environment = environment
        environment["NATIVEPIPE_SSH_CONNECTION"] = command.credentialID
        environment["NATIVEPIPE_SSH_AUTH_SESSION"] = auth.path
        child.environment = environment
        child.standardInput = input
        child.standardOutput = FileHandle.nullDevice
        child.standardError = output
        cancelled = false
        process = child
        defer {
            if child.isRunning { child.terminate() }
            process = nil
        }
        try await withTaskCancellationHandler {
            try Task.checkCancellation()
            try child.run()
            try input.fileHandleForReading.close()
            try output.fileHandleForWriting.close()
            guard fcntl(input.fileHandleForWriting.fileDescriptor, F_SETNOSIGPIPE, 1) == 0 else {
                throw POSIXError(.EIO)
            }
            let sending = Task.detached {
                defer { try? input.fileHandleForWriting.close() }
                try input.fileHandleForWriting.write(contentsOf: Data(batch.utf8))
            }
            let result = await Task.detached {
                defer { try? output.fileHandleForReading.close() }
                var diagnostic = Data()
                while let data = try? output.fileHandleForReading.read(upToCount: 4096), !data.isEmpty {
                    diagnostic.append(data)
                    if diagnostic.count > 8192 { diagnostic = diagnostic.suffix(8192) }
                }
                child.waitUntilExit()
                return (child.terminationStatus, String(decoding: diagnostic, as: UTF8.self))
            }.value
            _ = try? await sending.value
            if cancelled || Task.isCancelled { throw CancellationError() }
            guard result.0 == 0 else {
                throw RemoteError.message(result.1.isEmpty ? "The file transfer failed." : result.1)
            }
        } onCancel: {
            Task { @MainActor [weak self] in self?.cancel() }
        }
    }
}
