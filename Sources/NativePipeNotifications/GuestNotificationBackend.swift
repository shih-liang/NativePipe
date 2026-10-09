import Foundation
import NativePipeProtocol

/// Everything that talks to macOS Notification Center. It runs in the process
/// that owns the application's notification permission: LinPortal's resident
/// service for VMs and SSH, or the display process itself for standalone use.
@MainActor
public protocol GuestNotificationBackend: AnyObject {
    /// The current macOS state. With `prompt`, an undecided state first asks.
    func authorization(prompt: Bool) async -> GuestNotificationAuthorization
    /// `responsePath` is the posting process's response socket for clicks.
    func present(identifier: String, content: GuestNotificationContent, responsePath: String) async throws
    func withdraw(identifier: String)
    /// Registers (or with nil removes) one owner's in-process response handler
    /// and the directory of its response socket.
    func attach(owner: UUID, directory: URL?, responder: ((String, String?) -> Bool)?)
}
