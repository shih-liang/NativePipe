import AppKit
import Darwin
import Foundation
import NativePipeNotifications
import NativePipeProtocol

/// Notification Center identifies an application, not a VM process. A click
/// received by another instance is delivered to the live session that posted it.
@MainActor
final class GuestNotificationResponseRouter {
    typealias Response = GuestNotificationResponse
    nonisolated static let maximumPayload = GuestNotificationResponseForwarder.maximumPayload
    private let server: LocalSocketServer
    let url: URL
    let directory: URL

    /// Bundles other than this process's own that may forward a click here. The
    /// resident service presents notifications for the display processes.
    private static var trustedForwarders: Set<String> = []
    static func allowsForwarder(bundle: String?) -> Bool {
        bundle.map { trustedForwarders.contains($0) } ?? false
    }
    static func endpointDirectory(in directory: URL) -> URL {
        GuestNotificationResponseForwarder.endpointDirectory(in: directory)
    }
    static func trustForwarder(bundle: String) { trustedForwarders.insert(bundle) }

    init(directory: URL, receive: @escaping @MainActor (String, String?) -> Bool) throws {
        // Keep the endpoint short enough for sockaddr_un even in an App Group.
        self.directory = Self.endpointDirectory(in: directory)
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
                    peer == getpid() || (NSRunningApplication(processIdentifier: peer)?.bundleIdentifier).map {
                        $0 == expectedBundle || Self.allowsForwarder(bundle: $0)
                    } == true
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
        await GuestNotificationResponseForwarder.forward(response, path: path, permittedDirectories: permittedDirectories)
    }
}
