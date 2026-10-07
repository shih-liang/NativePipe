import Darwin
import Foundation
import FSKit
import NativePipeFileSharing
import XCTest
@testable import NativePipeFileSystem

@available(macOS 27.0, *)
final class SharedFilesItemLifetimeTests: XCTestCase {
    override func setUpWithError() throws {
        try super.setUpWithError()
        guard ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27 else {
            throw XCTSkip("Native FSKit sharing requires macOS 27 or later")
        }
    }

    private func volume(fileSystemTypeName: String = "nativepipe") throws -> (URL, SharedFilesVolume) {
        let directory = URL(fileURLWithPath: "/tmp/np-items-" + UUID().uuidString.prefix(8))
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])
        let manifest = directory.appendingPathComponent("volume.json")
        try JSONEncoder().encode(SharedFileVolumeDescriptor(id: UUID(), name: "Files", token: UUID()))
            .write(to: manifest)
        guard chmod(manifest.path, 0o600) == 0 else { throw POSIXError(.EACCES) }
        return (directory, SharedFilesVolume(client: try SharedFileVolumeClient(directory: directory), fileSystemTypeName: fileSystemTypeName))
    }

    func testStatFSMatchesTheMountingExtensionRegistration() throws {
        for typeName in ["nativepipe", "linportal"] {
            let (directory, volume) = try volume(fileSystemTypeName: typeName)
            defer { try? FileManager.default.removeItem(at: directory) }
            XCTAssertEqual(volume.volumeStatistics.fileSystemTypeName, typeName)
        }
    }

    func testReplacedNativeItemLivesUntilReclaimAndCannotRemoveItsReplacement() async throws {
        let (directory, volume) = try volume()
        defer { try? FileManager.default.removeItem(at: directory) }
        let initial = SharedFileItem(id: 3, parentID: 2, name: "file", kind: .file,
            size: 5, modified: nil, permissions: 0o600, version: Data([1]))
        let updated = SharedFileItem(id: 3, parentID: 2, name: "file", kind: .file,
            size: 6, modified: nil, permissions: 0o600, version: Data([2]))
        var previous: SharedFilesItem? = try volume.state.sync { try volume.item(initial) }
        weak let old = previous
        let replacement = try volume.state.sync { try volume.item(updated) }
        XCTAssertFalse(previous === replacement)
        previous = nil
        // The deferred native cache-revocation request may also hold the item.
        // Wait for that request to return before testing catalog ownership.
        volume.cacheUpdates.sync {}
        XCTAssertNotNil(old, "The kernel may still refer to this replaced FSItem")
        let eligible = volume.state.sync { old?.tryReclaim {} == true }
        try await reclaim(try XCTUnwrap(old), from: volume)
        volume.state.sync {}
        if eligible {
            XCTAssertNil(old, "An eligible reclaim must release the retired object")
        } else {
            XCTAssertNotNil(old, "The module must retain an item while FSKit declines reclaim")
        }
        XCTAssertTrue(try volume.state.sync { try volume.item(updated) } === replacement,
                      "Reclaiming an old version must not remove the current version")
    }

    func testDeactivatePreservesNativeItemsUntilReclaim() async throws {
        let (directory, volume) = try volume()
        defer { try? FileManager.default.removeItem(at: directory) }
        let metadata = SharedFileItem(id: 3, parentID: 2, name: "file", kind: .file,
            size: 5, modified: nil, permissions: 0o600, version: Data([1]))
        var item: SharedFilesItem? = try volume.state.sync { try volume.item(metadata) }
        weak let old = item
        item = nil
        let error: Error? = await withCheckedContinuation { continuation in
            volume.deactivateVolume(options: []) { continuation.resume(returning: $0) }
        }
        XCTAssertNil(error)
        XCTAssertNotNil(old, "Deactivation must not release objects before native reclaim")
        let eligible = volume.state.sync { old?.tryReclaim {} == true }
        try await reclaim(try XCTUnwrap(old), from: volume)
        volume.state.sync {}
        if eligible { XCTAssertNil(old) } else { XCTAssertNotNil(old) }
    }

    private func reclaim(_ item: FSItem, from volume: SharedFilesVolume) async throws {
        let error: Error? = await withCheckedContinuation { continuation in
            volume.reclaimItem(item) { continuation.resume(returning: $0) }
        }
        if let error { throw error }
    }
}
