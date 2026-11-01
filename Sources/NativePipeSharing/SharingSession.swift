import Foundation
import NativePipeProtocol
import NativePipeStrings

/// The one sharing backend behind every frontend (drag and drop, clipboard,
/// opening on the other side, file browsers) for one machine connection.
///
/// Frontends say what the user did; the session picks how the files travel:
/// - Mac → Linux: the guest's live drop mount (FUSE, edits reach the Mac file)
///   when it is attached, otherwise a copy over the file connection.
/// - Linux → Mac: the live shared volume (FSKit) when this build and Mac can
///   mount it, otherwise copies made over the file connection on demand.
@MainActor
public final class SharingSession {
    /// The file connection: FileRPC for a VM, SFTP for a remote machine.
    public let transport: any UserFileAccess
    /// Live Mac → Linux share: the guest path for a Mac file, or nil when the
    /// drop mount is not attached or cannot serve that item.
    public var guestShare: ((URL) async throws -> URL?)?
    /// Live Linux → Mac share.
    public var hostVolume: (any HostFileVolume)?

    private let cacheRoot: URL
    private var copies: [GuestFileSharePurpose: [URL]] = [:]
    private var stopped = false

    public init(transport: any UserFileAccess,
                guestShare: ((URL) async throws -> URL?)? = nil,
                hostVolume: (any HostFileVolume)? = nil) {
        self.transport = transport
        self.guestShare = guestShare
        self.hostVolume = hostVolume
        cacheRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("nativepipe-shared-" + UUID().uuidString, isDirectory: true)
    }

    // MARK: Mac → Linux

    /// Guest URLs for Mac files, in order. `liveShare: false` always copies,
    /// for items that must not outlive the gesture (promised temporary files).
    public func guestURLs(for hostFiles: [URL], purpose: GuestFileSharePurpose, liveShare: Bool = true,
                          progress: @escaping @Sendable (FileTransferProgress) -> Void = { _ in }) async throws -> [URL] {
        _ = try FileTransferURLs.decode(FileTransferURLs.encode(hostFiles))
        guard !stopped else { throw CancellationError() }
        var result = [URL?](repeating: nil, count: hostFiles.count)
        var copied: [(index: Int, url: URL)] = []
        for (index, url) in hostFiles.enumerated() {
            try Task.checkCancellation()
            if liveShare, let guestShare, let shared = try await guestShare(url) {
                result[index] = shared
            } else {
                copied.append((index, url))
            }
        }
        if !copied.isEmpty {
            let urls = try await transport.importFiles(copied.map(\.url), progress: progress)
            guard urls.count == copied.count else { throw FileRPC.Failure.protocolError }
            for (entry, url) in zip(copied, urls) { result[entry.index] = url }
        }
        return result.compactMap { $0 }
    }

    /// Opens Mac files in a Linux application, as Finder's Open With does on
    /// the Mac: the files reach the guest live where possible, otherwise as
    /// copies, and the application entry decides how it receives them.
    public func open(_ hostFiles: [URL], with application: String,
                     using applications: ApplicationClient) async throws -> Int32 {
        let urls = try await guestURLs(for: hostFiles, purpose: .open)
        guard urls.count == hostFiles.count else { throw FileRPC.Failure.protocolError }
        return try await applications.open(urls, with: application)
    }

    // MARK: Linux → Mac

    public enum HostFiles: Equatable {
        /// Mac URLs on the live shared volume, readable right away.
        case mounted([URL])
        /// The volume is unavailable: copy each guest item when it is needed.
        case deferred([URL])
    }

    /// How Mac apps can reach guest files for one purpose.
    public func hostFiles(for guestFiles: [URL], purpose: GuestFileSharePurpose) async throws -> HostFiles {
        _ = try FileTransferURLs.decode(FileTransferURLs.encode(guestFiles))
        guard !stopped else { throw CancellationError() }
        if let hostVolume {
            do {
                let urls = try await hostVolume.publish(guestFiles, using: transport, purpose: purpose)
                guard urls.count == guestFiles.count else { throw FileRPC.Failure.protocolError }
                _ = try FileTransferURLs.decode(FileTransferURLs.encode(urls))
                return .mounted(urls)
            } catch is HostFileVolumeUnavailable {}
        }
        return .deferred(guestFiles)
    }

    /// Mac URLs for guest items a person chose, for example to show them in
    /// Finder: the live volume when it is available, otherwise copies kept in
    /// one private folder under their guest names until the purpose is revoked.
    public func hostURLs(for guestFiles: [URL], purpose: GuestFileSharePurpose,
                         progress: @escaping @Sendable (FileTransferProgress) -> Void = { _ in }) async throws -> [URL] {
        let remotes: [URL]
        switch try await hostFiles(for: guestFiles, purpose: purpose) {
        case .mounted(let urls): return urls
        case .deferred(let deferred): remotes = deferred
        }
        let folder = try reserveCopyFolder(purpose: purpose)
        var names = Set<String>(), result: [URL] = []
        for remote in remotes {
            try Task.checkCancellation()
            let name = remote.lastPathComponent
            guard !["", "/", ".", ".."].contains(name) else { throw FileRPC.Failure.invalidPath }
            // Items from different guest folders may share a name.
            var parent = folder
            if !names.insert(name).inserted {
                let unique = folder.appendingPathComponent(UUID().uuidString, isDirectory: true)
                try await FileTransferLocalIO.perform {
                    try FileManager.default.createDirectory(at: unique, withIntermediateDirectories: false)
                }
                parent = unique
            }
            let local = parent.appendingPathComponent(name, isDirectory: remote.hasDirectoryPath)
            try await copyGuestItem(remote, to: local, progress: progress)
            result.append(local)
        }
        return result
    }

    /// True while copies made for a purpose (or any purpose) still exist.
    public func holdsCopies(for purpose: GuestFileSharePurpose? = nil) -> Bool {
        copies.contains { (purpose == nil || $0.key == purpose) && !$0.value.isEmpty }
    }

    /// Copies one guest item to an exact Mac URL, which must not exist yet.
    /// `promised` fulfils a file promise: only the promised URL is written,
    /// as AppKit requires of a sandboxed source; otherwise the item is staged
    /// beside the destination and moved into place when complete.
    public func copyGuestItem(_ remote: URL, to local: URL, promised: Bool = false,
                              progress: @escaping @Sendable (FileTransferProgress) -> Void = { _ in }) async throws {
        guard !stopped else { throw CancellationError() }
        if promised { try await transport.exportPromisedFile(remote, to: local) }
        else { try await transport.exportFile(remote, to: local, progress: progress) }
    }

    /// A new private folder for copies made for one purpose, removed with the
    /// purpose's other copies.
    public func reserveCopyFolder(purpose: GuestFileSharePurpose) throws -> URL {
        guard !stopped else { throw CancellationError() }
        let folder = cacheRoot.appendingPathComponent(purpose.rawValue, isDirectory: true)
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        copies[purpose, default: []].append(folder)
        return folder
    }

    /// A private Mac copy of one guest item, kept until the purpose is revoked
    /// or the session stops. Opened items stay until the session stops.
    public func temporaryCopy(of remote: URL, purpose: GuestFileSharePurpose,
                              progress: @escaping @Sendable (FileTransferProgress) -> Void = { _ in }) async throws -> URL {
        guard !stopped else { throw CancellationError() }
        let name = remote.lastPathComponent
        guard !["", "/", ".", ".."].contains(name) else { throw FileRPC.Failure.invalidPath }
        let folder = cacheRoot.appendingPathComponent(purpose.rawValue, isDirectory: true)
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try await FileTransferLocalIO.perform {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true,
                                                    attributes: [.posixPermissions: 0o700])
        }
        copies[purpose, default: []].append(folder)
        let destination = folder.appendingPathComponent(name, isDirectory: remote.hasDirectoryPath)
        do { try await transport.exportFile(remote, to: destination, progress: progress) }
        catch { remove([folder]); throw error }
        return destination
    }

    // MARK: Lifetime

    /// Withdraws what was shared for one purpose, or for all of them.
    public func revoke(_ purpose: GuestFileSharePurpose? = nil) {
        hostVolume?.revoke(purpose: purpose)
        for key in copies.keys where key != .open && (purpose == nil || key == purpose) {
            remove(copies.removeValue(forKey: key) ?? [])
        }
    }

    public func stop() {
        stopped = true
        hostVolume?.stop()
        copies.removeAll()
        let root = cacheRoot
        Task.detached(priority: .utility) { try? FileManager.default.removeItem(at: root) }
    }

    private func remove(_ folders: [URL]) {
        guard !folders.isEmpty else { return }
        Task.detached(priority: .utility) { for folder in folders { try? FileManager.default.removeItem(at: folder) } }
    }
}
