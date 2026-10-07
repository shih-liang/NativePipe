import Foundation
import NativePipeProtocol

@MainActor
public final class RemoteUserFileAccess: UserFileRangeAccess {
    let command: SSHCommand
    let environment: [String: String]
    private let makeSession: @MainActor () -> SFTPFileSystem
    private var rangeSession: SFTPFileSystem?
    private var rangeTail: Task<Void, Never>?
    private var rangeGeneration: UInt64 = 0
    private var rangeCancellations: [UUID: @Sendable () -> Void] = [:]
    public init(command: SSHCommand, environment: [String: String]?) {
        self.command = command
        self.environment = environment ?? ProcessInfo.processInfo.environment
        let environment = self.environment
        makeSession = { SFTPFileSystem(command: command, environment: environment, reuseConnection: true) }
    }
    init(makeSession: @escaping @MainActor () -> SFTPFileSystem) {
        command = SSHCommand(destination: "fixture", application: []); environment = [:]
        self.makeSession = makeSession
    }
    public func metadata(for remote: URL) async throws -> UserFileMetadata {
        try UserFileRange.validate(remote)
        return try await rangeOperation { try await $0.snapshot(path: remote.path) }
    }
    public func contents(of remote: URL) async throws -> [UserFileMetadata] {
        try UserFileRange.validate(remote)
        return try await rangeOperation { try await $0.rangeContents(path: remote.path) }
    }
    public func read(_ remote: URL, offset: UInt64, length: Int, expectedVersion: Data) async throws -> Data {
        try UserFileRange.validate(remote, offset: offset, length: length)
        return try await rangeOperation { try await $0.readRange(path: remote.path, offset: offset, length: length, expectedVersion: expectedVersion) }
    }
    public func closeRangeAccess() {
        rangeGeneration &+= 1
        for cancel in rangeCancellations.values { cancel() }
        rangeSession?.close(); rangeSession = nil
    }
    private func rangeOperation<Value: Sendable>(_ operation: @escaping @MainActor (SFTPFileSystem) async throws -> Value) async throws -> Value {
        try Task.checkCancellation()
        let predecessor = rangeTail, generation = rangeGeneration
        let delivery = RangeDelivery<Value>()
        let worker = Task { @MainActor in
            await predecessor?.value
            try Task.checkCancellation()
            guard generation == self.rangeGeneration else { throw CancellationError() }
            let session = self.rangeSession ?? self.makeSession()
            self.rangeSession = session
            do { return try await operation(session) }
            catch {
                session.close()
                if self.rangeSession === session { self.rangeSession = nil }
                if case SFTPFileSystemError.sourceChanged(let path) = error { throw FileRPC.Failure.sourceChanged(path) }
                throw error
            }
        }
        let identifier = UUID()
        rangeCancellations[identifier] = { worker.cancel(); delivery.finish(.failure(CancellationError())) }
        defer { rangeCancellations.removeValue(forKey: identifier) }
        rangeTail = Task { _ = try? await worker.value }
        Task {
            do { delivery.finish(.success(try await worker.value)) }
            catch { delivery.finish(.failure(error)) }
        }
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { delivery.bind($0) }
        } onCancel: {
            // A queued reader must finish promptly without canceling the SSH
            // operation that currently owns the stream ahead of it.
            worker.cancel(); delivery.finish(.failure(CancellationError()))
        }
    }
    public func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL] {
        try await importFiles(urls, shareDirectories: shareDirectories, progress: { _ in })
    }
    public func importFiles(_ urls: [URL], shareDirectories: Bool,
                     progress: @escaping @Sendable (FileTransferProgress) -> Void) async throws -> [URL] {
        _ = try FileTransferURLs.decode(FileTransferURLs.encode(urls))
        let tracker = FileTransferProgressTracker(progress: progress)
        var result: [URL] = []
        for url in urls {
            try Task.checkCancellation()
            guard url.isFileURL else { throw FileRPC.Failure.invalidPath }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            let isDirectory = try await FileTransferLocalIO.perform { try url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true }
            try Task.checkCancellation()
            let parent = "/tmp/nativepipe-drop-" + UUID().uuidString
            let remote = parent + "/" + url.lastPathComponent
            let session = makeSession()
            defer { session.close() }
            try await session.createDirectory(path: parent)
            try await session.transfer(direction: .upload, local: url, remote: remote, recursive: true) { sample in
                tracker.report(sample.bytesTransferred, relativePath: url.lastPathComponent +
                               (sample.relativePath.isEmpty ? "" : "/" + sample.relativePath))
            }
            tracker.finishFile()
            result.append(try RemoteFileURL.make(remote, isDirectory: isDirectory))
        }
        tracker.finish()
        return result
    }
    public func exportFile(_ remote: URL, to local: URL) async throws {
        try await exportFile(remote, to: local, progress: { _ in })
    }
    public func exportFile(_ remote: URL, to local: URL,
                    progress: @escaping @Sendable (FileTransferProgress) -> Void) async throws {
        _ = try FileTransferURLs.decode(FileTransferURLs.encode([remote]))
        let parent = local.deletingLastPathComponent()
        let scoped = parent.startAccessingSecurityScopedResource()
        defer { if scoped { parent.stopAccessingSecurityScopedResource() } }
        let session = makeSession()
        defer { session.close() }
        try await session.transfer(direction: .download, local: local, remote: remote.path,
                                   recursive: true, progress: progress)
    }
}

private final class RangeDelivery<Value: Sendable>: @unchecked Sendable {
    private let lock = NSLock()
    private var continuation: CheckedContinuation<Value, Error>?
    private var result: Result<Value, Error>?
    func bind(_ continuation: CheckedContinuation<Value, Error>) {
        lock.lock()
        if let result { lock.unlock(); continuation.resume(with: result) }
        else { self.continuation = continuation; lock.unlock() }
    }
    func finish(_ result: Result<Value, Error>) {
        lock.lock()
        guard self.result == nil else { lock.unlock(); return }
        self.result = result
        let continuation = self.continuation; self.continuation = nil
        lock.unlock(); continuation?.resume(with: result)
    }
}
