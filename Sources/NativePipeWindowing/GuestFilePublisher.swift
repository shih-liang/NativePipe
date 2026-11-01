import Foundation
import NativePipeProtocol
import NativePipeStrings

/// Embedding applications can expose selected guest files as mounted macOS
/// URLs. The connection owner retains and revokes their filesystem lifetime;
/// registering names never needs to copy their contents.
public typealias GuestFilePublisher = @MainActor ([URL], any UserFileAccess, GuestFileSharePurpose) async throws -> [URL]

/// The publisher itself never downloads a private copy. Callers that need
/// the files anyway (drags) copy them over the file connection instead.
public enum GuestFileSharingError: LocalizedError {
    case unavailable
    public var errorDescription: String? {
        NPText("The virtual file sharing backend is not available.")
    }
}
