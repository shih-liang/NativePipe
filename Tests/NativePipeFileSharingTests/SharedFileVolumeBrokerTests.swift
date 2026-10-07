import Foundation
import Darwin
import AppKit
import NativePipeProtocol
import XCTest
@testable import NativePipeFileSharing

@available(macOS 27.0, *)
@MainActor
final class SharedFileVolumeBrokerTests: XCTestCase {
    override func setUpWithError() throws {
        try super.setUpWithError()
        guard ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27 else {
            throw XCTSkip("Native FSKit sharing requires macOS 27 or later")
        }
    }

    private func broker(_ access: SharedRangeFixture) throws -> SharedFileVolumeBroker {
        let parent = URL(fileURLWithPath: "/tmp/lpfs-" + UUID().uuidString.prefix(8))
        try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: false)
        addTeardownBlock { try? FileManager.default.removeItem(at: parent) }
        return SharedFileVolumeBroker(directory: parent.appendingPathComponent("v"), name: "Test Machine",
            mount: { _ in URL(fileURLWithPath: "/Volumes/LinPortal-Fixture-" + UUID().uuidString) }, unmount: { _ in })
    }
    func testShareOnlyPublishesMetadataAndReadTransfersRequestedBytesOfLargeFile() async throws {
        let access = SharedRangeFixture(), broker = try broker(access)
        defer { broker.stop() }
        let file = URL(fileURLWithPath: "/selected/huge.bin")
        access.add(file, size: 1 << 40, version: Data(repeating: 7, count: 300))
        let urls = try await broker.publish([file], using: access)
        XCTAssertEqual(urls.map(\.lastPathComponent), ["huge.bin"])
        XCTAssertTrue(access.reads.isEmpty)
        XCTAssertEqual(access.exports, 0)
        let client = try SharedFileVolumeClient(directory: broker.directory)
        let entries = try await client.children(id: 2)
        XCTAssertEqual(entries.count, 1)
        XCTAssertEqual(entries[0].size, 1 << 40)
        XCTAssertTrue(access.reads.isEmpty)
        let bytes = try await client.read(id: entries[0].id, offset: 123_456, length: 5, version: entries[0].version)
        XCTAssertEqual(bytes, Data("hello".utf8))
        XCTAssertEqual(access.reads.count, 1)
        XCTAssertEqual(access.reads[0].offset, 123_456)
        XCTAssertEqual(access.reads[0].length, 5)
        XCTAssertEqual(access.exports, 0)
    }
    func testDirectoryExpandsLazilyAndRejectsUnselectedSubtreeWithoutPartialCatalog() async throws {
        let access = SharedRangeFixture(), broker = try broker(access)
        defer { broker.stop() }
        let folder = URL(fileURLWithPath: "/selected/folder", isDirectory: true)
        let child = folder.appendingPathComponent("child.txt")
        access.add(folder, kind: .directory)
        access.add(child)
        _ = try await broker.publish([folder], using: access)
        XCTAssertEqual(access.enumerations, 0)
        let client = try SharedFileVolumeClient(directory: broker.directory)
        let root = try await client.children(id: 2)
        access.listings[folder] = [access.metadataValues[child]!, .init(url: URL(fileURLWithPath: "/unselected/secret"),
            kind: .file, size: 1, modified: nil, permissions: 0o600, version: Data([1]))]
        do { _ = try await client.children(id: root[0].id); XCTFail("An unrelated path must not become visible") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .EACCES) }
        access.listings[folder] = [access.metadataValues[child]!]
        let children = try await client.children(id: root[0].id)
        XCTAssertEqual(children.map(\.name), ["child.txt"])
        XCTAssertEqual(access.enumerations, 2)
        XCTAssertTrue(access.reads.isEmpty)
    }
    func testDuplicatesAreNamedSafelyAndRepeatShareReusesIdentity() async throws {
        let access = SharedRangeFixture(), broker = try broker(access)
        defer { broker.stop() }
        let name = "a." + String(repeating: "x", count: 253)
        let first = URL(fileURLWithPath: "/first/" + name), second = URL(fileURLWithPath: "/second/" + name)
        access.add(first); access.add(second)
        _ = try await broker.publish([first, second], using: access)
        _ = try await broker.publish([first], using: access)
        let entries = try await SharedFileVolumeClient(directory: broker.directory).children(id: 2)
        XCTAssertEqual(entries.count, 2)
        XCTAssertEqual(Set(entries.map(\.name)).count, 2)
        XCTAssertTrue(entries.allSatisfy { $0.name.utf8.count <= 255 })
    }
    func testChangedSourceOldReadFailsFreshMetadataSucceedsAndRevokeExpiresIDs() async throws {
        let access = SharedRangeFixture(), broker = try broker(access)
        defer { broker.stop() }
        let file = URL(fileURLWithPath: "/selected/file.txt")
        access.add(file)
        _ = try await broker.publish([file], using: access)
        let client = try SharedFileVolumeClient(directory: broker.directory)
        let original = try await client.children(id: 2)[0]
        access.add(file, version: Data([2]))
        do { _ = try await client.read(id: original.id, offset: 0, length: 5, version: original.version); XCTFail("Old versions must fail") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .ESTALE) }
        let fresh = try await client.item(id: original.id)
        XCTAssertEqual(fresh.version, Data([2]))
        let freshBytes = try await client.read(id: fresh.id, offset: 0, length: 5, version: fresh.version)
        XCTAssertEqual(freshBytes, Data("hello".utf8))
        broker.revoke()
        let revoked = try await client.children(id: 2)
        XCTAssertTrue(revoked.isEmpty)
        do { _ = try await client.item(id: original.id); XCTFail("Revoked IDs must fail") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .ESTALE) }
        _ = try await broker.publish([file], using: access)
        let next = try await client.children(id: 2)[0]
        XCTAssertNotEqual(next.id, original.id)
        XCTAssertEqual(access.closed, 1)
    }
    func testRevokedMountCompletionCannotClearOrPublishNewGenerationMount() async throws {
        let access = SharedRangeFixture()
        let parent = URL(fileURLWithPath: "/tmp/lpfs-" + UUID().uuidString.prefix(8))
        try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: parent) }
        var completions: [CheckedContinuation<URL, Error>] = [], unmounted: [URL] = []
        let broker = SharedFileVolumeBroker(directory: parent.appendingPathComponent("v"), name: "Race",
            mount: { _ in try await withCheckedThrowingContinuation { completions.append($0) } }, unmount: { unmounted.append($0) })
        defer { broker.stop() }
        let file = URL(fileURLWithPath: "/selected/file")
        access.add(file)
        let first = Task { try await broker.publish([file], using: access) }
        try await wait { completions.count == 1 }
        broker.revoke()
        let second = Task { try await broker.publish([file], using: access) }
        try await wait { completions.count == 2 }
        let stale = URL(fileURLWithPath: "/Volumes/Old-" + UUID().uuidString, isDirectory: true)
        completions[0].resume(returning: stale)
        do { _ = try await first.value; XCTFail("Old mount must not publish") } catch {}
        let third = Task { try await broker.publish([file], using: access) }
        await Task.yield()
        XCTAssertEqual(completions.count, 2, "The old completion must not erase the new in-flight mount")
        let fresh = URL(fileURLWithPath: "/Volumes/New-" + UUID().uuidString, isDirectory: true)
        completions[1].resume(returning: fresh)
        let secondURL = try await second.value[0], thirdURL = try await third.value[0]
        XCTAssertEqual(secondURL.deletingLastPathComponent(), fresh)
        XCTAssertEqual(thirdURL.deletingLastPathComponent(), fresh)
        XCTAssertEqual(unmounted, [stale])
    }
    func testLinuxErrorsMapToDarwinAndInvalidTimestampCannotEnterCatalog() async throws {
        XCTAssertEqual(SharedFileVolumeBroker.errorCode(FileRPC.Failure.remote(38)), ENOSYS)
        XCTAssertEqual(SharedFileVolumeBroker.errorCode(FileRPC.Failure.remote(116)), ESTALE)
        let access = SharedRangeFixture(), broker = try broker(access)
        defer { broker.stop() }
        let file = URL(fileURLWithPath: "/selected/invalid")
        access.add(file)
        access.metadataValues[file]?.modified = Date(timeIntervalSince1970: Double(Int.max))
        do { _ = try await broker.publish([file], using: access); XCTFail("Invalid timestamps must be rejected") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .EINVAL) }
    }
    func testClipboardPolicyRevokesOnlyClipboardSharesAndNoIDsReappear() async throws {
        let access = SharedRangeFixture(), broker = try broker(access)
        defer { broker.stop() }
        let clipboard = URL(fileURLWithPath: "/selected/clipboard"), opened = URL(fileURLWithPath: "/selected/opened")
        access.add(clipboard); access.add(opened)
        _ = try await broker.publish([clipboard], using: access, purpose: .clipboard)
        _ = try await broker.publish([opened], using: access, purpose: .open)
        let client = try SharedFileVolumeClient(directory: broker.directory)
        let before = try await client.children(id: 2)
        let oldID = try XCTUnwrap(before.first { $0.name == "clipboard" }?.id)
        broker.revoke(purpose: .clipboard)
        let after = try await client.children(id: 2)
        XCTAssertEqual(after.map(\.name), ["opened"])
        XCTAssertEqual(access.closed, 0, "An approved open share using the same transport must remain available")
        do { _ = try await client.item(id: oldID); XCTFail("Clipboard policy must revoke an already published capability") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .ESTALE) }
    }
    func testFinderEjectRevokesPublishedFilesAndNextShareGetsANewMount() async throws {
        let access = SharedRangeFixture()
        let parent = URL(fileURLWithPath: "/tmp/lpfs-" + UUID().uuidString.prefix(8))
        try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: parent) }
        var mounts = 0, changes: [Bool] = []
        let broker = SharedFileVolumeBroker(directory: parent.appendingPathComponent("v"), name: "Eject",
            mount: { _ in mounts += 1; return URL(fileURLWithPath: "/Volumes/Test-\(UUID())", isDirectory: true) }, unmount: { _ in })
        defer { broker.stop() }
        broker.publicationDidChange = { [weak broker] in changes.append(broker?.hasPublishedFiles ?? false) }
        let file = URL(fileURLWithPath: "/selected/file")
        access.add(file)
        let first = try await broker.publish([file], using: access)[0]
        let client = try SharedFileVolumeClient(directory: broker.directory)
        let old = try await client.children(id: 2)[0]
        XCTAssertTrue(broker.hasPublishedFiles)
        NSWorkspace.shared.notificationCenter.post(name: NSWorkspace.didUnmountNotification, object: nil,
            userInfo: [NSWorkspace.volumeURLUserInfoKey: URL(fileURLWithPath: "/Volumes/Other")])
        XCTAssertTrue(broker.hasPublishedFiles, "An unrelated mount must stay live")
        NSWorkspace.shared.notificationCenter.post(name: NSWorkspace.didUnmountNotification, object: nil,
            userInfo: [NSWorkspace.volumeURLUserInfoKey: first.deletingLastPathComponent()])
        XCTAssertFalse(broker.hasPublishedFiles)
        XCTAssertEqual(access.closed, 1)
        do { _ = try await client.item(id: old.id); XCTFail("Ejected catalog IDs must fail") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .ESTALE) }
        let next = try await broker.publish([file], using: access)[0]
        XCTAssertNotEqual(next.deletingLastPathComponent(), first.deletingLastPathComponent())
        XCTAssertEqual(mounts, 2)
        XCTAssertEqual(changes, [true, false, true])
    }
    func testLinuxCanonicalEquivalentNamesKeepDistinctSourceBytesAndFinderPaths() async throws {
        let access = SharedRangeFixture(), broker = try broker(access)
        defer { broker.stop() }
        var components = URLComponents()
        components.scheme = "file"
        components.path = "/selected/\u{e9}.txt"
        let first = try XCTUnwrap(components.url)
        components.path = "/selected/e\u{301}.txt"
        let second = try XCTUnwrap(components.url)
        access.add(first, version: Data([1])); access.add(second, version: Data([2]))
        let urls = try await broker.publish([first, second], using: access)
        XCTAssertNotEqual(Data(urls[0].path.utf8), Data(urls[1].path.utf8))
        let client = try SharedFileVolumeClient(directory: broker.directory)
        let items = try await client.children(id: 2)
        XCTAssertEqual(items.count, 2)
        XCTAssertEqual(Set(items.map(\.version)), [Data([1]), Data([2])])
        XCTAssertEqual(Set(items.map { Data($0.name.utf8) }), Set(urls.map { Data($0.lastPathComponent.utf8) }))
        for item in items {
            _ = try await client.read(id: item.id, offset: 0, length: 5, version: item.version)
        }
        XCTAssertEqual(access.reads.count, 2)
    }
    func testFirstNativeMountCanSeeAndReadPendingSelectionBeforePublicationCommits() async throws {
        let access = SharedRangeFixture()
        let parent = URL(fileURLWithPath: "/tmp/lpfs-" + UUID().uuidString.prefix(8))
        try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: parent) }
        var mountEntries: [SharedFileItem] = [], bytes: Data?
        let broker = SharedFileVolumeBroker(directory: parent.appendingPathComponent("v"), name: "Mount",
            mount: { directory in
                let client = try SharedFileVolumeClient(directory: directory)
                mountEntries = try await client.children(id: 2)
                let file = try XCTUnwrap(mountEntries.first)
                bytes = try await client.read(id: file.id, offset: 0, length: 5, version: file.version)
                return URL(fileURLWithPath: "/Volumes/Test-\(UUID())")
            }, unmount: { _ in })
        defer { broker.stop() }
        let file = URL(fileURLWithPath: "/selected/file")
        access.add(file)
        XCTAssertFalse(broker.hasPublishedFiles)
        _ = try await broker.publish([file], using: access)
        XCTAssertEqual(mountEntries.map(\.name), ["file"])
        XCTAssertEqual(bytes, Data("hello".utf8))
        XCTAssertTrue(broker.hasPublishedFiles)
    }
    func testReenumerationOfReplacedDirectoryExpiresItsPreviousDescendants() async throws {
        let access = SharedRangeFixture(), broker = try broker(access)
        defer { broker.stop() }
        let folder = URL(fileURLWithPath: "/selected/folder", isDirectory: true)
        let subfolder = folder.appendingPathComponent("subfolder", isDirectory: true)
        let child = subfolder.appendingPathComponent("child")
        access.add(folder, kind: .directory); access.add(subfolder, kind: .directory); access.add(child)
        access.listings[folder] = [access.metadataValues[subfolder]!]
        access.listings[subfolder] = [access.metadataValues[child]!]
        _ = try await broker.publish([folder], using: access)
        let client = try SharedFileVolumeClient(directory: broker.directory)
        let root = try await client.children(id: 2)[0]
        let sub = try await client.children(id: root.id)[0]
        let oldChild = try await client.children(id: sub.id)[0]
        access.add(subfolder, kind: .directory, version: Data([2]))
        access.listings[folder] = [access.metadataValues[subfolder]!]
        _ = try await client.children(id: root.id)
        do { _ = try await client.item(id: oldChild.id); XCTFail("A replaced directory must revoke its previous descendants") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .ESTALE) }
    }
    func testClipboardRevocationDuringFirstMountImmediatelyHidesPendingSelection() async throws {
        let access = SharedRangeFixture()
        let parent = URL(fileURLWithPath: "/tmp/lpfs-" + UUID().uuidString.prefix(8))
        try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: parent) }
        var completion: CheckedContinuation<URL, Error>?
        let broker = SharedFileVolumeBroker(directory: parent.appendingPathComponent("v"), name: "Pending",
            mount: { _ in try await withCheckedThrowingContinuation { completion = $0 } }, unmount: { _ in })
        defer { broker.stop() }
        let file = URL(fileURLWithPath: "/selected/clipboard")
        access.add(file)
        let task = Task { try await broker.publish([file], using: access, purpose: .clipboard) }
        try await wait { completion != nil }
        let client = try SharedFileVolumeClient(directory: broker.directory)
        let entries = try await client.children(id: 2)
        XCTAssertEqual(entries.count, 1)
        XCTAssertFalse(broker.hasPublishedFiles)
        broker.revoke(purpose: .clipboard)
        let revoked = try await client.children(id: 2)
        XCTAssertTrue(revoked.isEmpty)
        XCTAssertEqual(access.closed, 1)
        completion?.resume(returning: URL(fileURLWithPath: "/Volumes/Test-\(UUID())"))
        do { _ = try await task.value; XCTFail("A revoked pending publication must fail") }
        catch is CancellationError {} catch { XCTFail("Unexpected publication error: \(error)") }
        XCTAssertFalse(broker.hasPublishedFiles)
    }
    private func wait(_ predicate: () -> Bool) async throws {
        let deadline = ContinuousClock.now + .seconds(2)
        while !predicate(), ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(5)) }
        guard predicate() else { throw POSIXError(.ETIMEDOUT) }
    }
}

@MainActor
private final class SharedRangeFixture: UserFileRangeAccess {
    var metadataValues: [URL: UserFileMetadata] = [:]
    var listings: [URL: [UserFileMetadata]] = [:]
    var reads: [(offset: UInt64, length: Int)] = []
    var exports = 0, enumerations = 0, closed = 0
    func add(_ url: URL, kind: UserFileMetadata.Kind = .file, size: UInt64 = 5, version: Data = Data([1])) {
        metadataValues[url] = .init(url: url, kind: kind, size: size, modified: nil, permissions: 0o600, version: version)
    }
    func metadata(for remote: URL) async throws -> UserFileMetadata {
        guard let value = metadataValues[remote] else { throw POSIXError(.ENOENT) }; return value
    }
    func contents(of remote: URL) async throws -> [UserFileMetadata] { enumerations += 1; return listings[remote] ?? [] }
    func read(_ remote: URL, offset: UInt64, length: Int, expectedVersion: Data) async throws -> Data {
        guard try await metadata(for: remote).version == expectedVersion else { throw FileRPC.Failure.sourceChanged(remote.path) }
        reads.append((offset, length)); return Data("hello".utf8.prefix(length))
    }
    func closeRangeAccess() { closed += 1 }
    func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL] { throw POSIXError(.ENOTSUP) }
    func exportFile(_ remote: URL, to local: URL) async throws { exports += 1; throw POSIXError(.ENOTSUP) }
}
