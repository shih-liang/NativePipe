import AppKit
import Darwin
import Foundation
import NativePipeProtocol

/// Notification Center identifies an application, not a VM process. A click
/// received by another instance is delivered to the live session that posted it.
@MainActor
final class GuestNotificationResponseRouter {
    struct Response: Codable, Sendable {
        let identifier: String
        let action: String?
        var isValid: Bool {
            identifier.hasPrefix("nativepipe.guest.") && identifier.utf8.count <= 256
                && (action.map { $0.utf8.count <= GuestNotificationPolicy.maximumActionKeyLength } ?? true)
        }
    }
    nonisolated static let maximumPayload = 2048
    private let server: LocalSocketServer
    let url: URL
    let directory: URL

    init(directory: URL, receive: @escaping @MainActor (String, String?) -> Bool) throws {
        // Keep the endpoint short enough for sockaddr_un even in an App Group.
        self.directory = directory.standardizedFileURL.appendingPathComponent(".n", isDirectory: true)
        try FileManager.default.createDirectory(at: self.directory, withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        var info = stat()
        guard lstat(self.directory.path, &info) == 0, info.st_mode & S_IFMT == S_IFDIR,
              info.st_uid == getuid(), info.st_mode & 0o077 == 0 else { throw POSIXError(.EACCES) }
        url = self.directory.appendingPathComponent("n" + UUID().uuidString.replacingOccurrences(of: "-", with: "").prefix(16))
        server = LocalSocketServer(url: url, maximumConnections: 8)
        let expectedBundle = Bundle.main.bundleIdentifier
        try server.start { connection in
            do {
                let peer = try await connection.processIdentifier()
                let permitted = await MainActor.run {
                    peer == getpid() || (expectedBundle != nil
                        && NSRunningApplication(processIdentifier: peer)?.bundleIdentifier == expectedBundle)
                }
                guard permitted else { return }
                let deadline: DispatchTime = .now() + .seconds(2)
                let count = try WireFormat.decodeHeader(await connection.readExactly(WireFormat.headerSize, deadline: deadline))
                guard count > 0, count <= Self.maximumPayload else { return }
                let response = try JSONDecoder().decode(Response.self, from: await connection.readExactly(count, deadline: deadline))
                guard response.isValid else { return }
                let accepted = await receive(response.identifier, response.action)
                try await connection.write(Data([accepted ? 1 : 0]), deadline: deadline)
            } catch { /* Dead or malformed peers do not own a notification. */ }
        }
        guard chmod(url.path, 0o600) == 0 else { server.stop(); throw POSIXError(.EACCES) }
    }

    func stop() {
        server.stop()
        _ = unlink(url.appendingPathExtension("lock").path)
    }

    static func forward(_ response: Response, path: String, permittedDirectories: Set<URL>) async -> Bool {
        guard response.isValid else { return false }
        let url = URL(fileURLWithPath: path).standardizedFileURL
        let name = url.lastPathComponent
        guard name.count == 17, name.first == "n", name.dropFirst().allSatisfy({ $0.isHexDigit }),
              permittedDirectories.contains(url.deletingLastPathComponent()) else { return false }
        var info = stat()
        guard lstat(url.path, &info) == 0, info.st_mode & S_IFMT == S_IFSOCK,
              info.st_uid == getuid(), info.st_mode & 0o077 == 0 else { return false }
        do {
            let payload = try JSONEncoder().encode(response)
            guard payload.count <= maximumPayload else { return false }
            let deadline: DispatchTime = .now() + .seconds(2)
            let connection = try await SocketConnection.connect(to: url, deadline: deadline)
            defer { connection.close() }
            try await connection.write(WireFormat.frame(payload: payload), deadline: deadline)
            return try await connection.readExactly(1, deadline: deadline) == Data([1])
        } catch { return false }
    }
}
