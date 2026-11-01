import Foundation
import NativePipeProtocol
import NativePipeSharing

/// A shared volume driven by a test closure, recording revocations.
@MainActor
final class ClosureHostVolume: HostFileVolume {
    typealias Publish = @MainActor ([URL], any UserFileAccess, GuestFileSharePurpose) async throws -> [URL]
    var publishHandler: Publish
    private(set) var revoked: [GuestFileSharePurpose?] = []
    private(set) var stopped = false

    init(_ publish: @escaping Publish) { publishHandler = publish }

    func publish(_ files: [URL], using access: any UserFileAccess,
                 purpose: GuestFileSharePurpose) async throws -> [URL] {
        try await publishHandler(files, access, purpose)
    }
    func revoke(purpose: GuestFileSharePurpose?) { revoked.append(purpose) }
    func stop() { stopped = true }
}

/// A volume that this build cannot mount, so the session copies instead.
@MainActor
func unavailableVolume() -> ClosureHostVolume {
    ClosureHostVolume { _, _, _ in throw HostFileVolumeUnavailable("No FSKit in tests") }
}
