import Foundation
import NativePipeProtocol

@MainActor
final class RemoteUserFileAccess: UserFileAccess {
    let command: SSHCommand
    let environment: [String: String]
    private let makeSession: @MainActor () -> SFTPFileSystem
    init(command: SSHCommand, environment: [String: String]?) {
        self.command = command
        self.environment = environment ?? ProcessInfo.processInfo.environment
        let environment = self.environment
        makeSession = { SFTPFileSystem(command: command, environment: environment, reuseConnection: true) }
    }
    init(makeSession: @escaping @MainActor () -> SFTPFileSystem) {
        command = SSHCommand(destination: "fixture", application: []); environment = [:]
        self.makeSession = makeSession
    }
    func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL] {
        try await importFiles(urls, shareDirectories: shareDirectories, progress: { _ in })
    }
    func importFiles(_ urls: [URL], shareDirectories: Bool,
                     progress: @escaping @Sendable (FileTransferProgress) -> Void) async throws -> [URL] {
        _ = try FileTransferURLs.decode(FileTransferURLs.encode(urls))
        let tracker = FileTransferProgressTracker(progress: progress)
        var result: [URL] = []
        for url in urls {
            try Task.checkCancellation()
            guard url.isFileURL else { throw FileRPC.Failure.invalidPath }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
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
            let isDirectory = try url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true
            result.append(URL(fileURLWithPath: remote, isDirectory: isDirectory))
        }
        tracker.finish()
        return result
    }
    func exportFile(_ remote: URL, to local: URL) async throws {
        try await exportFile(remote, to: local, progress: { _ in })
    }
    func exportFile(_ remote: URL, to local: URL,
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
