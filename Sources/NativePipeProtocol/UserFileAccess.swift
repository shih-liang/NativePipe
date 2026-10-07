import Foundation
import Darwin

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
public final class FileRPCUserAccess: UserFileAccess {
    private let rpc: FileRPC
    public var shareFile: ((URL) async throws -> URL?)?
    public init(rpc: FileRPC) { self.rpc = rpc }

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
            let isDirectory = try url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true
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
            result.append(URL(fileURLWithPath: target, isDirectory: isDirectory))
        }
        tracker.finish()
        return result
    }

    private func upload(_ local: URL, to remote: String, relativePath: String, depth: Int,
                        tracker: FileTransferProgressTracker) async throws {
        try Task.checkCancellation()
        guard depth < 64 else { throw FileRPC.Failure.invalidPath }
        let info = try local.resourceValues(forKeys: [.isDirectoryKey, .isRegularFileKey, .isSymbolicLinkKey])
        guard info.isSymbolicLink != true else { throw FileRPC.Failure.invalidPath }
        if info.isDirectory == true {
            try await rpc.createDirectory(remote)
            for child in try FileManager.default.contentsOfDirectory(at: local, includingPropertiesForKeys: nil) {
                try await upload(child, to: remote + "/" + child.lastPathComponent,
                                 relativePath: relativePath + "/" + child.lastPathComponent,
                                 depth: depth + 1, tracker: tracker)
            }
        } else {
            guard info.isRegularFile == true else { throw FileRPC.Failure.invalidPath }
            let file = try FileHandle(forReadingFrom: local)
            defer { try? file.close() }
            var info = stat()
            guard fstat(file.fileDescriptor, &info) == 0, (info.st_mode & S_IFMT) == S_IFREG else {
                throw FileRPC.Failure.invalidPath
            }
            tracker.report(0, relativePath: relativePath)
            try await rpc.upload(file, to: remote, mode: UInt32(info.st_mode & 0o777)) { completed, _ in
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
        var destination = stat()
        if lstat(local.path, &destination) == 0 { throw FileRPC.Failure.local(EEXIST) }
        guard errno == ENOENT else { throw FileRPC.Failure.local(errno) }
        let info = try await rpc.stat(remote.path)
        let tracker = FileTransferProgressTracker(totalBytes: info.isRegular ? info.size : nil, progress: progress)
        let staging = local.deletingLastPathComponent().appendingPathComponent(".nativepipe-transfer-" + UUID().uuidString)
        do {
            try await download(remote.path, to: staging, relativePath: "", depth: 0, tracker: tracker)
            try Task.checkCancellation()
            guard renamex_np(staging.path, local.path, UInt32(RENAME_EXCL)) == 0 else { throw FileRPC.Failure.local(errno) }
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
            guard mkdir(local.path, 0o700) == 0 else { throw FileRPC.Failure.local(errno) }
            for entry in try await rpc.read(remote).entries {
                try await download(remote + "/" + entry.name,
                    to: local.appendingPathComponent(entry.name),
                    relativePath: relativePath.isEmpty ? entry.name : relativePath + "/" + entry.name,
                    depth: depth + 1, tracker: tracker)
            }
            guard chmod(local.path, mode_t(info.permissions & 0o777)) == 0 else { throw FileRPC.Failure.local(errno) }
        } else {
            guard info.isRegular else { throw FileRPC.Failure.invalidPath }
            let fd = open(local.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0o600)
            guard fd >= 0 else { throw FileRPC.Failure.local(errno) }
            let file = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
            defer { try? file.close() }
            tracker.report(0, relativePath: relativePath)
            try await rpc.download(remote, to: file) { completed, _ in
                tracker.report(completed, relativePath: relativePath)
            }
            guard fchmod(fd, mode_t(info.permissions & 0o777)) == 0 else { throw FileRPC.Failure.local(errno) }
            tracker.finishFile()
        }
    }
}
