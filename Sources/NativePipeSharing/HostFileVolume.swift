import Foundation
import NativePipeProtocol
import NativePipeStrings

/// A live Mac view of Linux files: the FSKit shared volume. Finder and other
/// Mac apps read the files on demand through it; nothing is copied first.
/// `NativePipeFileSharing` provides the implementation, so frontends never
/// depend on FSKit.
@MainActor
public protocol HostFileVolume: AnyObject {
    /// Mounted Mac URLs for the guest files, in order. Throws
    /// `HostFileVolumeUnavailable` when this build or Mac cannot mount it.
    func publish(_ files: [URL], using access: any UserFileAccess,
                 purpose: GuestFileSharePurpose) async throws -> [URL]
    /// Withdraws published files: one purpose's, or all of them.
    func revoke(purpose: GuestFileSharePurpose?)
    func stop()
}

/// The shared volume cannot be used at all here (no FSKit authorization, the
/// file system extension is turned off, macOS too old, …). The backend then
/// copies the files instead; any other error is a real failure.
public struct HostFileVolumeUnavailable: LocalizedError {
    public let reason: String
    public init(_ reason: String) { self.reason = reason }
    public var errorDescription: String? { reason }
}
