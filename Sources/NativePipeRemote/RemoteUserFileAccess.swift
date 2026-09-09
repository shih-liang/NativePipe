import Foundation
import NativePipeProtocol

@MainActor
final class RemoteUserFileAccess: UserFileAccess {
    let command: SSHCommand
    let environment: [String: String]
    init(command: SSHCommand, environment: [String: String]?) {
        self.command = command
        self.environment = environment ?? ProcessInfo.processInfo.environment
    }
    func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL] {
        var result: [URL] = []
        for url in urls {
            try Task.checkCancellation()
            guard url.isFileURL else { throw FileRPC.Failure.invalidPath }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            let remote = "/tmp/nativepipe-drop-" + UUID().uuidString + "/" + url.lastPathComponent
            try await SFTPTransfer().run(command: command, direction: .upload, local: url,
                remote: remote, environment: environment, recursive: true, createParent: true)
            result.append(URL(fileURLWithPath: remote))
        }
        return result
    }
    func exportFile(_ remote: URL, to local: URL) async throws {
        _ = try FileTransferURLs.decode(FileTransferURLs.encode([remote]))
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("nativepipe-receive-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let received = directory.appendingPathComponent(remote.lastPathComponent)
        try await SFTPTransfer().run(command: command, direction: .download, local: received,
            remote: remote.path, environment: environment, recursive: true)
        // moveItem refuses to overwrite another file, including a symlink.
        try FileManager.default.moveItem(at: received, to: local)
    }
}
