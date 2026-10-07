import Foundation
import Darwin

/// Linux paths are byte-sensitive. Foundation's native file URL initializers
/// and component appending decompose Unicode according to macOS filesystem
/// conventions, so use URI construction for paths owned by a guest instead.
public enum RemoteFileURL {
    public static func make(_ path: String, isDirectory: Bool = false) throws -> URL {
        guard path.hasPrefix("/"), !path.contains("\0"), path.utf8.count <= 4095 else { throw FileRPC.Failure.invalidPath }
        var components = URLComponents()
        components.scheme = "file"; components.host = ""
        components.path = isDirectory && !path.hasSuffix("/") ? path + "/" : path
        guard let url = components.url else { throw FileRPC.Failure.invalidPath }
        return url
    }
    public static func appending(_ name: String, to directory: URL, isDirectory: Bool = false) throws -> URL {
        guard directory.isFileURL, directory.host == nil || directory.host == "" || directory.host == "localhost",
              directory.query == nil, directory.fragment == nil, directory.user == nil,
              !name.isEmpty, name != ".", name != "..", !name.contains("/"),
              !name.contains("\0"), name.utf8.count <= 255 else { throw FileRPC.Failure.invalidPath }
        let path = directory.path == "/" ? "/" + name : directory.path + "/" + name
        return try make(path, isDirectory: isDirectory)
    }
}

/// Metadata for a selected remote item. `version` is opaque and belongs to this
/// path and transport; pass it back unchanged when reading a range.
public struct UserFileMetadata: Codable, Hashable, Sendable {
    public enum Kind: String, Codable, Sendable { case file, directory, symbolicLink, other }
    public var url: URL
    public var kind: Kind
    public var size: UInt64
    public var modified: Date?
    public var permissions: UInt32
    public var version: Data
    public init(url: URL, kind: Kind, size: UInt64, modified: Date?, permissions: UInt32, version: Data) {
        self.url = url; self.kind = kind; self.size = size; self.modified = modified
        self.permissions = permissions & 0o7777; self.version = version
    }
}

/// On-demand file access uses the same user-authorized transport as ordinary
/// transfers. Individual reads are bounded; the total file size is not.
@MainActor
public protocol UserFileRangeAccess: UserFileAccess {
    func metadata(for remote: URL) async throws -> UserFileMetadata
    func contents(of remote: URL) async throws -> [UserFileMetadata]
    func read(_ remote: URL, offset: UInt64, length: Int, expectedVersion: Data) async throws -> Data
    func closeRangeAccess()
}
public extension UserFileRangeAccess { func closeRangeAccess() {} }

public enum UserFileRange {
    public static let maximumReadLength = 1_048_576
    public static let maximumVersionLength = 8192
    public static func validate(_ remote: URL, offset: UInt64 = 0, length: Int = 0) throws {
        guard remote.isFileURL, remote.host == nil || remote.host == "" || remote.host == "localhost",
              remote.query == nil, remote.fragment == nil, remote.user == nil,
              remote.path.hasPrefix("/"), !remote.path.contains("\0"), remote.path.utf8.count <= 4095,
              remote.path.split(separator: "/").allSatisfy({ $0 != "." && $0 != ".." }),
              length >= 0, length <= maximumReadLength, offset <= UInt64(Int64.max),
              UInt64(length) <= UInt64(Int64.max) - offset else { throw FileRPC.Failure.invalidPath }
    }
}

/// Local paths may themselves be served by this process's filesystem broker.
/// Never block its main actor on a read, lookup, or directory enumeration.
public enum FileTransferLocalIO {
    public static func perform<Value: Sendable>(_ operation: @escaping @Sendable () throws -> Value) async throws -> Value {
        let worker = Task.detached(priority: .utility) {
            try Task.checkCancellation()
            let value = try operation()
            return value
        }
        return try await withTaskCancellationHandler { try await worker.value } onCancel: { worker.cancel() }
    }
}

/// User-selected files only. Implementations are user-vsock (VM) and SFTP
/// (Remote); the pasteboard and AppKit drag lifecycle are shared by both.
@MainActor
public protocol UserFileAccess: AnyObject {
    func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL]
    func exportFile(_ remote: URL, to local: URL) async throws
    func importFiles(_ urls: [URL], shareDirectories: Bool,
                     progress: @escaping @Sendable (FileTransferProgress) -> Void) async throws -> [URL]
    func exportFile(_ remote: URL, to local: URL,
                    progress: @escaping @Sendable (FileTransferProgress) -> Void) async throws
}

public extension UserFileAccess {
    func importFiles(_ urls: [URL]) async throws -> [URL] {
        try await importFiles(urls, shareDirectories: true)
    }
    func importFiles(_ urls: [URL], shareDirectories: Bool,
                     progress: @escaping @Sendable (FileTransferProgress) -> Void) async throws -> [URL] {
        let result = try await importFiles(urls, shareDirectories: shareDirectories)
        progress(.init(bytesTransferred: 0, isComplete: true))
        return result
    }
    func exportFile(_ remote: URL, to local: URL,
                    progress: @escaping @Sendable (FileTransferProgress) -> Void) async throws {
        try await exportFile(remote, to: local)
        progress(.init(bytesTransferred: 0, isComplete: true))
    }
    /// One destination per selected item; duplicates fail before any transfer.
    func exportFiles(_ remotes: [URL], to directory: URL,
                     progress: @escaping @Sendable (FileTransferProgress) -> Void = { _ in }) async throws -> [URL] {
        _ = try FileTransferURLs.decode(FileTransferURLs.encode(remotes))
        var names = Set<String>()
        guard remotes.allSatisfy({ names.insert($0.lastPathComponent).inserted }) else { throw FileRPC.Failure.invalidPath }
        let tracker = FileTransferProgressTracker(progress: progress)
        var result: [URL] = []
        for remote in remotes {
            try Task.checkCancellation()
            let local = directory.appendingPathComponent(remote.lastPathComponent)
            try await exportFile(remote, to: local) { sample in
                tracker.report(sample.bytesTransferred, relativePath: remote.lastPathComponent +
                               (sample.relativePath.isEmpty ? "" : "/" + sample.relativePath))
            }
            tracker.finishFile(); result.append(local)
        }
        tracker.finish()
        return result
    }
}

public enum FileTransferURLs {
    public static func decode(_ data: Data) throws -> [URL] {
        guard data.count <= 1024 * 1024, let text = String(data: data, encoding: .utf8) else {
            throw FileRPC.Failure.invalidPath
        }
        let lines = text.split(whereSeparator: \.isNewline).filter { !$0.hasPrefix("#") }
        guard !lines.isEmpty, lines.count <= 1024 else { throw FileRPC.Failure.invalidPath }
        return try lines.map {
            guard let parts = URLComponents(string: String($0)),
                  let path = parts.percentEncodedPath.removingPercentEncoding,
                  !path.contains("\0"), let url = parts.url, url.isFileURL,
                  url.host == nil || url.host == "" || url.host == "localhost",
                  url.query == nil, url.fragment == nil, url.user == nil,
                  url.path.hasPrefix("/"), !url.path.contains("\0"),
                  url.path.utf8.count <= 4095, !url.lastPathComponent.isEmpty,
                  url.lastPathComponent != "/", url.lastPathComponent != ".", url.lastPathComponent != ".."
            else { throw FileRPC.Failure.invalidPath }
            return url
        }
    }
    public static func encode(_ urls: [URL]) -> Data {
        Data((urls.map(\.absoluteString).joined(separator: "\r\n") + "\r\n").utf8)
    }
}

/// No root RPC is used by desktop clients. A VM can optionally clone/share
/// host files first; every failed clone falls back to this streaming path.
@MainActor
public final class FileRPCUserAccess: UserFileRangeAccess {
    private let rpc: FileRPC
    private var rangeCancellations: [UUID: @Sendable () -> Void] = [:]
    private var rangeGeneration: UInt64 = 0
    public var shareFile: ((URL) async throws -> URL?)?
    public init(rpc: FileRPC) { self.rpc = rpc }

    public func metadata(for remote: URL) async throws -> UserFileMetadata {
        try UserFileRange.validate(remote)
        return try await rangeOperation { try await self.rpc.snapshot(remote.path) }
    }
    public func contents(of remote: URL) async throws -> [UserFileMetadata] {
        try UserFileRange.validate(remote)
        return try await rangeOperation {
            let parent = try await self.rpc.snapshot(remote.path)
            guard parent.kind == .directory else { throw FileRPC.Failure.local(ENOTDIR) }
            var children: [UserFileMetadata] = []
            for entry in try await self.rpc.directoryEntries(remote.path) {
                try Task.checkCancellation()
                // Unsupported links/special files are never followed by lazy access.
                if entry.fileType != .unknown && !entry.isRegular && !entry.isDirectory { continue }
                children.append(try await self.rpc.snapshot(RemoteFileURL.appending(entry.name, to: remote).path))
            }
            let current = try await self.rpc.snapshot(remote.path)
            guard current.version == parent.version else { throw FileRPC.Failure.sourceChanged(remote.path) }
            return children
        }
    }
    public func read(_ remote: URL, offset: UInt64, length: Int, expectedVersion: Data) async throws -> Data {
        try UserFileRange.validate(remote, offset: offset, length: length)
        return try await rangeOperation { try await self.rpc.readRange(remote.path, offset: offset, length: length, expectedVersion: expectedVersion) }
    }
    public func closeRangeAccess() {
        rangeGeneration &+= 1
        for cancel in rangeCancellations.values { cancel() }
    }
    private func rangeOperation<Value: Sendable>(_ operation: @escaping @MainActor () async throws -> Value) async throws -> Value {
        try Task.checkCancellation()
        let generation = rangeGeneration
        let worker = Task { @MainActor in
            try Task.checkCancellation()
            return try await operation()
        }
        let identifier = UUID()
        rangeCancellations[identifier] = { worker.cancel() }
        defer { rangeCancellations.removeValue(forKey: identifier) }
        let value = try await withTaskCancellationHandler { try await worker.value } onCancel: { worker.cancel() }
        guard generation == rangeGeneration else { throw CancellationError() }
        return value
    }

    public func importFiles(_ urls: [URL], shareDirectories: Bool = true) async throws -> [URL] {
        try await importFiles(urls, shareDirectories: shareDirectories, progress: { _ in })
    }
    public func importFiles(_ urls: [URL], shareDirectories: Bool,
                            progress: @escaping @Sendable (FileTransferProgress) -> Void) async throws -> [URL] {
        _ = try FileTransferURLs.decode(FileTransferURLs.encode(urls))
        let tracker = FileTransferProgressTracker(progress: progress)
        var result: [URL] = []
        var directory: String?
        for url in urls {
            try Task.checkCancellation()
            guard url.isFileURL else { throw FileRPC.Failure.invalidPath }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            let isDirectory = try await FileTransferLocalIO.perform { try url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true }
            try Task.checkCancellation()
            if shareDirectories || !isDirectory,
               let shared = try await shareFile?(url) { result.append(shared); continue }
            if directory == nil {
                let path = "/tmp/nativepipe-drop-" + UUID().uuidString
                try await rpc.createDirectory(path)
                directory = path
            }
            // Separate subdirectories allow two selected files with the same name.
            let parent = directory! + "/" + String(result.count)
            try await rpc.createDirectory(parent)
            let target = parent + "/" + url.lastPathComponent
            try await upload(url, to: target, relativePath: url.lastPathComponent, depth: 0, tracker: tracker)
            result.append(try RemoteFileURL.make(target, isDirectory: isDirectory))
        }
        tracker.finish()
        return result
    }

    /// Upload to an exact destination; publication never overwrites or merges.
    public func uploadFile(_ local: URL, to remote: URL,
                           progress: @escaping @Sendable (FileTransferProgress) -> Void = { _ in }) async throws {
        try UserFileRange.validate(remote)
        guard local.isFileURL, !["", "/", ".", ".."].contains(remote.lastPathComponent) else { throw FileRPC.Failure.invalidPath }
        let scoped = local.startAccessingSecurityScopedResource()
        defer { if scoped { local.stopAccessingSecurityScopedResource() } }
        let parent = remote.deletingLastPathComponent()
        let stagingURL = try RemoteFileURL.appending(".nativepipe-upload-" + UUID().uuidString, to: parent, isDirectory: true)
        let staging = try await rpc.createUploadStaging(stagingURL.path)
        let tracker = FileTransferProgressTracker(progress: progress)
        do {
            try await upload(local, to: staging.path + "/.payload", relativePath: local.lastPathComponent,
                             depth: 0, noFollow: true, tracker: tracker)
            try Task.checkCancellation()
            let rpc = rpc
            // Once complete, finish the atomic commit and report its real result.
            try await Task.detached(priority: .utility) { try await rpc.publishStaging(staging, to: remote.path) }.value
        } catch {
            let rpc = rpc
            // Cleanup is a fresh operation so cancellation cannot cancel it.
            await Task.detached(priority: .utility) { try? await rpc.discardStaging(staging) }.value
            throw error
        }
        tracker.finish()
    }

    private func upload(_ local: URL, to remote: String, relativePath: String, depth: Int,
                        noFollow: Bool = false, tracker: FileTransferProgressTracker) async throws {
        try Task.checkCancellation()
        guard depth < 64 else { throw FileRPC.Failure.invalidPath }
        let info = try await FileTransferLocalIO.perform {
            let values = try local.resourceValues(forKeys: [.isDirectoryKey, .isRegularFileKey, .isSymbolicLinkKey])
            return (values.isDirectory == true, values.isRegularFile == true, values.isSymbolicLink == true)
        }
        guard !info.2 else { throw FileRPC.Failure.invalidPath }
        if info.0 {
            try await rpc.createDirectory(remote, noFollow: noFollow)
            let children = try await FileTransferLocalIO.perform { try FileManager.default.contentsOfDirectory(at: local, includingPropertiesForKeys: nil) }
            for child in children {
                try await upload(child, to: remote + "/" + child.lastPathComponent,
                                 relativePath: relativePath + "/" + child.lastPathComponent,
                                 depth: depth + 1, noFollow: noFollow, tracker: tracker)
            }
        } else {
            guard info.1 else { throw FileRPC.Failure.invalidPath }
            let (file, mode) = try await FileTransferLocalIO.perform {
                let descriptor = open(local.path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)
                guard descriptor >= 0 else { throw FileRPC.Failure.local(errno) }
                let file = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
                var info = stat()
                guard fstat(file.fileDescriptor, &info) == 0, (info.st_mode & S_IFMT) == S_IFREG else {
                    try? file.close(); throw FileRPC.Failure.invalidPath
                }
                return (file, UInt32(info.st_mode & 0o777))
            }
            defer { try? file.close() }
            tracker.report(0, relativePath: relativePath)
            try await rpc.upload(file, to: remote, mode: mode, noFollow: noFollow) { completed, _ in
                tracker.report(completed, relativePath: relativePath)
            }
            tracker.finishFile()
        }
    }

    public func exportFile(_ remote: URL, to local: URL) async throws {
        try await exportFile(remote, to: local, progress: { _ in })
    }
    public func exportFile(_ remote: URL, to local: URL,
                           progress: @escaping @Sendable (FileTransferProgress) -> Void) async throws {
        _ = try FileTransferURLs.decode(FileTransferURLs.encode([remote]))
        guard local.isFileURL, !["", "/", ".", ".."].contains(local.lastPathComponent) else { throw FileRPC.Failure.invalidPath }
        let parent = local.deletingLastPathComponent()
        let scoped = parent.startAccessingSecurityScopedResource()
        defer { if scoped { parent.stopAccessingSecurityScopedResource() } }
        try await FileTransferLocalIO.perform {
            var destination = stat()
            if lstat(local.path, &destination) == 0 { throw FileRPC.Failure.local(EEXIST) }
            guard errno == ENOENT else { throw FileRPC.Failure.local(errno) }
        }
        let info = try await rpc.stat(remote.path)
        let tracker = FileTransferProgressTracker(totalBytes: info.isRegular ? info.size : nil, progress: progress)
        let staging = local.deletingLastPathComponent().appendingPathComponent(".nativepipe-transfer-" + UUID().uuidString)
        do {
            try await download(remote.path, to: staging, relativePath: "", depth: 0, tracker: tracker)
            try Task.checkCancellation()
            try await FileTransferLocalIO.perform {
                guard renamex_np(staging.path, local.path, UInt32(RENAME_EXCL)) == 0 else { throw FileRPC.Failure.local(errno) }
            }
        } catch {
            // Removing a large failed tree must not block the UI. Detached
            // cleanup finishes even when its transfer was cancelled, and the
            // original transport/publication error remains the result.
            await Task.detached(priority: .utility) {
                try? FileManager.default.removeItem(at: staging)
            }.value
            throw error
        }
        tracker.finish()
    }

    private func download(_ remote: String, to local: URL, relativePath: String, depth: Int,
                          tracker: FileTransferProgressTracker) async throws {
        try Task.checkCancellation()
        guard depth < 64 else { throw FileRPC.Failure.invalidPath }
        let info = try await rpc.stat(remote)
        if info.isDirectory {
            // Never merge into an existing tree or follow a destination symlink.
            try await FileTransferLocalIO.perform {
                guard mkdir(local.path, 0o700) == 0 else { throw FileRPC.Failure.local(errno) }
            }
            for entry in try await rpc.read(remote).entries {
                try await download(remote + "/" + entry.name,
                    to: local.appendingPathComponent(entry.name),
                    relativePath: relativePath.isEmpty ? entry.name : relativePath + "/" + entry.name,
                    depth: depth + 1, tracker: tracker)
            }
            try await FileTransferLocalIO.perform {
                guard chmod(local.path, mode_t(info.permissions & 0o777)) == 0 else { throw FileRPC.Failure.local(errno) }
            }
        } else {
            guard info.isRegular else { throw FileRPC.Failure.invalidPath }
            let fd = try await FileTransferLocalIO.perform {
                let fd = open(local.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0o600)
                guard fd >= 0 else { throw FileRPC.Failure.local(errno) }
                return fd
            }
            let file = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
            defer { try? file.close() }
            tracker.report(0, relativePath: relativePath)
            try await rpc.download(remote, to: file) { completed, _ in
                tracker.report(completed, relativePath: relativePath)
            }
            try await FileTransferLocalIO.perform {
                guard fchmod(fd, mode_t(info.permissions & 0o777)) == 0 else { throw FileRPC.Failure.local(errno) }
            }
            tracker.finishFile()
        }
    }
}
