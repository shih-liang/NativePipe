import Foundation
import NativePipeProtocol
import NativePipeStrings

/// Embedding applications can expose selected guest files as mounted macOS
/// URLs. The connection owner retains and revokes their filesystem lifetime;
/// registering names never needs to copy their contents.
public typealias GuestFilePublisher = @MainActor ([URL], any UserFileAccess, GuestFileSharePurpose) async throws -> [URL]

/// Missing filesystem support is reported; sharing never downloads a private copy.
public enum GuestFileSharingError: LocalizedError {
    case unavailable
    public var errorDescription: String? {
        NPText("The virtual file sharing backend is not available.")
    }
}
