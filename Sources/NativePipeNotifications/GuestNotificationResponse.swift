import Darwin
import Foundation
import NativePipeProtocol

/// A notification click, relayed to the process that posted the notification.
public struct GuestNotificationResponse: Codable, Sendable {
    public let identifier: String
    public let action: String?
    public init(identifier: String, action: String?) { self.identifier = identifier; self.action = action }
    public var isValid: Bool {
        identifier.hasPrefix(GuestNotificationLimits.identifierPrefix)
            && identifier.utf8.count <= GuestNotificationLimits.identifierBytes
            && (action.map { $0.utf8.count <= GuestNotificationLimits.actionKey } ?? true)
    }
}

/// Notification Center identifies an application, not a display process. A click
/// received by the presenting process is delivered to the live session that posted it.
public enum GuestNotificationResponseForwarder {
    public static let maximumPayload = 2048

    /// Response sockets live in a private directory below the IPC directory,
    /// short enough for sockaddr_un even in an App Group.
    public static func endpointDirectory(in directory: URL) -> URL {
        directory.standardizedFileURL.appendingPathComponent(".n", isDirectory: true)
    }

    /// A response socket is named by the router, never chosen by a guest, and
    /// sits only in a directory this process was configured to use.
    public static func isPermittedEndpoint(_ path: String, in permittedDirectories: Set<URL>) -> Bool {
        let url = URL(fileURLWithPath: path).standardizedFileURL
        let name = url.lastPathComponent
        return name.count == 17 && name.first == "n" && name.dropFirst().allSatisfy { $0.isHexDigit }
            && permittedDirectories.contains(url.deletingLastPathComponent())
    }

    public static func forward(_ response: GuestNotificationResponse, path: String, permittedDirectories: Set<URL>) async -> Bool {
        guard response.isValid else { return false }
        guard isPermittedEndpoint(path, in: permittedDirectories) else { return false }
        let url = URL(fileURLWithPath: path).standardizedFileURL
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
