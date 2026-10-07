import Darwin
import Foundation
import NativePipeProtocol
import XCTest
@testable import NativePipeWindowing

@MainActor
private final class ReceivedFiles: UserFileAccess {
    var exports = 0
    var publications = 0
    var directory = false
    var size = 6
    var contents = Data("hello!".utf8)
    var fails = false
    var afterPublication: (() -> Void)?
    var sources: [URL] = []
    var mounted: [URL] = []
    func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL] { urls }
    func exportFile(_ remote: URL, to local: URL) async throws {
        exports += 1; XCTFail("np-open must never export a private guest copy")
        throw FileRPC.Failure.protocolError
    }
    func publish(_ urls: [URL], in root: URL) async throws -> [URL] {
        publications += 1; sources += urls
        if fails { throw FileRPC.Failure.protocolError }
        let directory = directory, size = size, contents = contents
        let result = try await FileTransferLocalIO.perform {
            try urls.map { remote in
                let folder = root.appendingPathComponent("mounted-" + UUID().uuidString, isDirectory: true)
                try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                let source = folder.appendingPathComponent(remote.lastPathComponent, isDirectory: directory)
                if directory {
                    try FileManager.default.createDirectory(at: source, withIntermediateDirectories: false)
                    try contents.write(to: source.appendingPathComponent("child.txt"))
                } else {
                    try contents.write(to: source)
                    let file = try FileHandle(forWritingTo: source)
                    defer { try? file.close() }
                    try file.truncate(atOffset: UInt64(size))
                }
                return source
            }
        }
        mounted += result; afterPublication?()
        return result
    }
}

@MainActor
final class GuestOpenCoordinatorTests: XCTestCase {
    private var root: URL!
    private var received: ReceivedFiles!
    private var opened: [URL] = []
    private var marked: [URL] = []
    private var prompts: [GuestOpenPrompt] = []

    override func setUp() {
        root = FileManager.default.temporaryDirectory.appendingPathComponent("host-open-" + UUID().uuidString)
        received = ReceivedFiles()
        opened = []; marked = []; prompts = []
    }
    override func tearDown() { try? FileManager.default.removeItem(at: root) }

    private func coordinator(enabled: @escaping () -> Bool = { true },
        shares: @escaping () -> [GuestOpenPolicy.SharedFolder] = { [] },
        action: @escaping (GuestOpenPrompt) async -> GuestOpenAction = { _ in .open },
        markFails: Bool = false, afterQuarantine: (() -> Void)? = nil,
        fileSharingAvailable: Bool = true, publishGuestFiles: GuestFilePublisher? = nil) -> GuestOpenCoordinator {
        let publisher: GuestFilePublisher? = !fileSharingAvailable ? nil : publishGuestFiles ?? { [self] urls, _, purpose in
            XCTAssertEqual(purpose, .open)
            return try await received.publish(urls, in: root)
        }
        return GuestOpenCoordinator(machineName: "Test Linux", isEnabled: enabled, shares: shares,
            fileAccess: received,
            confirm: { [self] prompt in prompts.append(prompt); return await action(prompt) },
            open: { [self] url in opened.append(url); return true },
            quarantine: { [self] url in
                if markFails { throw FileRPC.Failure.protocolError }
                marked.append(url); afterQuarantine?()
            }, transferProgress: { _ in }, publishGuestFiles: publisher)
    }

    func testMountPublicationHappensOnlyAfterReceiveApprovalAndNeedsNoPrivateCopy() async throws {
        let directory = root.appendingPathComponent("mounted")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let file = directory.appendingPathComponent("document.txt")
        try Data("document".utf8).write(to: file)
        var publications = 0
        let publish: GuestFilePublisher = { urls, _, purpose in
            XCTAssertEqual(purpose, .open); XCTAssertEqual(urls.map(\.path), ["/home/test/document.txt"])
            publications += 1; return [file]
        }
        let denied = await coordinator(action: { _ in .cancel }, publishGuestFiles: publish)
            .handle(.init(kind: .file, value: "/home/test/document.txt"))
        XCTAssertEqual(denied.status, .refused); XCTAssertEqual(publications, 0)
        let allowed = await coordinator(publishGuestFiles: publish)
            .handle(.init(kind: .file, value: "/home/test/document.txt"))
        XCTAssertEqual(allowed.status, .opened); XCTAssertEqual(publications, 1); XCTAssertEqual(received.exports, 0)
        XCTAssertEqual(opened, [file]); XCTAssertTrue(marked.isEmpty)
    }

    func testMissingPublisherNeverFallsBackToExportForOpenOrSave() async throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let destination = root.appendingPathComponent("saved.txt")
        for action in [GuestOpenAction.open, .save(destination)] {
            let response = await coordinator(action: { _ in action }, fileSharingAvailable: false)
                .handle(.init(kind: .file, value: "/guest/file.txt"))
            XCTAssertEqual(response.status, .failed); XCTAssertNotNil(response.message)
        }
        XCTAssertEqual(received.publications, 0); XCTAssertEqual(received.exports, 0)
        XCTAssertTrue(opened.isEmpty); XCTAssertTrue(marked.isEmpty)
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
    }

    func testPublishedExecutableStillRequiresIndividualExecutionApproval() async throws {
        received.contents = Data("#!/bin/sh\ntrue\n".utf8); received.size = received.contents.count
        let response = await coordinator(action: { prompt in
            if case .execute = prompt { return .cancel }; return .open
        }).handle(.init(kind: .file, value: "/guest/tool.command"))
        XCTAssertEqual(response.status, .refused)
        XCTAssertEqual(prompts.count, 2); XCTAssertEqual(received.publications, 1); XCTAssertEqual(received.exports, 0)
        XCTAssertTrue(opened.isEmpty); XCTAssertTrue(marked.isEmpty)
        XCTAssertTrue(FileManager.default.fileExists(atPath: try XCTUnwrap(received.mounted.first).path))
    }

    func testExplicitSaveCopiesMountedSourceWithoutAnotherRemoteTransfer() async throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let destination = root.appendingPathComponent("saved.txt")
        let response = await coordinator(action: { _ in .save(destination) })
            .handle(.init(kind: .file, value: "/guest/file.txt"))
        XCTAssertEqual(response.status, .opened); XCTAssertEqual(received.publications, 1); XCTAssertEqual(received.exports, 0)
        XCTAssertEqual(try Data(contentsOf: destination), received.contents)
        XCTAssertTrue(opened.isEmpty); XCTAssertEqual(marked, [destination])
    }

    func testSaveNeverOverwritesExistingDestination() async throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let destination = root.appendingPathComponent("existing.txt")
        try Data("keep".utf8).write(to: destination)
        let response = await coordinator(action: { _ in .save(destination) }).handle(.init(kind: .file, value: "/guest/file.txt"))
        XCTAssertEqual(response.status, .failed); XCTAssertEqual(try Data(contentsOf: destination), Data("keep".utf8))
        XCTAssertTrue(marked.isEmpty); XCTAssertTrue(opened.isEmpty); XCTAssertEqual(received.exports, 0)
    }

    func testLinuxUnicodeNamesKeepTheirExactBytesForPublicationAndSave() async throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        for name in ["Résumé.txt".precomposedStringWithCanonicalMapping, "Résumé.txt".decomposedStringWithCanonicalMapping] {
            let path = "/guest/" + name
            let result = await coordinator().handle(.init(kind: .file, value: path))
            XCTAssertEqual(result.status, .opened)
            XCTAssertEqual(Array(try XCTUnwrap(received.sources.last).path.utf8), Array(path.utf8))
            let destination = root.appendingPathComponent(UUID().uuidString)
            let saved = await coordinator(action: { _ in .save(destination) }).handle(.init(kind: .file, value: path))
            XCTAssertEqual(saved.status, .opened)
            XCTAssertEqual(Array(try XCTUnwrap(received.sources.last).path.utf8), Array(path.utf8))
        }
        XCTAssertEqual(received.exports, 0)
    }

    func testNoURLIsOpenedWithoutApproval() async {
        let result = await coordinator(action: { _ in .cancel }).handle(.init(kind: .url, value: "https://user:password@example.com"))
        XCTAssertEqual(result.status, .refused); XCTAssertTrue(opened.isEmpty); XCTAssertEqual(prompts.count, 1)
    }
    func testCredentialURLAndMailAttachmentReachLaunchServicesOnlyAfterApproval() async {
        for value in ["https://user:password@example.com", "mailto:test@example.com?attach=/tmp/test.pdf", "ssh://host"] {
            let result = await coordinator(fileSharingAvailable: false).handle(.init(kind: .url, value: value))
            XCTAssertEqual(result.status, .opened); XCTAssertEqual(opened.last?.absoluteString, value)
        }
    }
    func testDisabledFeatureDoesNotPromptOrPublish() async {
        let result = await coordinator(enabled: { false }).handle(.init(kind: .file, value: "/home/test/file"))
        XCTAssertEqual(result.status, .disabled); XCTAssertTrue(prompts.isEmpty); XCTAssertEqual(received.publications, 0)
    }
    func testCancellingInitialFileApprovalRegistersNoFiles() async {
        let result = await coordinator(action: { _ in .cancel }).handle(.init(kind: .file, value: "/home/test/tool.pkg"))
        XCTAssertEqual(result.status, .refused); XCTAssertEqual(received.publications, 0); XCTAssertEqual(received.exports, 0)
        XCTAssertTrue(opened.isEmpty); XCTAssertFalse(FileManager.default.fileExists(atPath: root.path))
    }
    func testLargeFileOpensMountedSourceWithoutASizeCeilingOrDownload() async throws {
        received.size = 64 * 1024 * 1024 + 1
        let result = await coordinator().handle(.init(kind: .file, value: "/home/test/large.dat"))
        XCTAssertEqual(result.status, .opened)
        XCTAssertEqual(try XCTUnwrap(opened.first).resourceValues(forKeys: [.fileSizeKey]).fileSize, received.size)
        XCTAssertEqual(received.exports, 0); XCTAssertTrue(marked.isEmpty)
    }
    func testFoldersOpenThroughTheSameMountedInterface() async throws {
        received.directory = true
        let result = await coordinator().handle(.init(kind: .file, value: "/home/test/folder"))
        XCTAssertEqual(result.status, .opened)
        XCTAssertEqual(try Data(contentsOf: XCTUnwrap(opened.first).appendingPathComponent("child.txt")), received.contents)
        XCTAssertEqual(prompts.count, 1); XCTAssertEqual(received.exports, 0); XCTAssertTrue(marked.isEmpty)
    }
    func testSavingAProgramCopiesItWithoutRunningIt() async throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let destination = root.appendingPathComponent("tool.command")
        let result = await coordinator(action: { _ in .save(destination) }).handle(.init(kind: .file, value: "/home/test/tool.command"))
        XCTAssertEqual(result.status, .opened); XCTAssertTrue(opened.isEmpty)
        XCTAssertEqual(try Data(contentsOf: destination), received.contents)
        XCTAssertEqual(prompts.count, 1); XCTAssertEqual(received.exports, 0)
    }
    func testRevokingShareDuringApprovalPreventsPublication() async {
        var shares = [GuestOpenPolicy.SharedFolder(tag: "docs", hostRoot: URL(fileURLWithPath: "/tmp/docs"))]
        let result = await coordinator(shares: { shares }, action: { _ in shares = []; return .open })
            .handle(.init(kind: .file, value: "/mnt/linportal/docs/file.txt"))
        XCTAssertEqual(result.status, .refused); XCTAssertEqual(received.publications, 0); XCTAssertTrue(opened.isEmpty)
    }
    func testRevokingShareDuringPublicationPreventsOpening() async {
        var shares = [GuestOpenPolicy.SharedFolder(tag: "docs", hostRoot: URL(fileURLWithPath: "/tmp/docs"))]
        received.afterPublication = { shares = [] }
        let result = await coordinator(shares: { shares }).handle(.init(kind: .file, value: "/mnt/linportal/docs/file.txt"))
        XCTAssertEqual(result.status, .refused); XCTAssertTrue(opened.isEmpty); XCTAssertEqual(received.exports, 0)
    }
    func testPublicationOrSaveQuarantineFailureNeverOpensAnythingAndCleansOnlyOurCopy() async throws {
        received.fails = true
        let failed = await coordinator().handle(.init(kind: .file, value: "/home/test/file.txt"))
        XCTAssertEqual(failed.status, .failed)
        received.fails = false
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let destination = root.appendingPathComponent("saved.txt")
        let unmarked = await coordinator(action: { _ in .save(destination) }, markFails: true)
            .handle(.init(kind: .file, value: "/home/test/file.txt"))
        XCTAssertEqual(unmarked.status, .failed); XCTAssertTrue(opened.isEmpty)
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: try XCTUnwrap(received.mounted.last).path))
    }
    func testRevokingShareAfterSaveRemovesCopyAndRetainsSource() async throws {
        var shares = [GuestOpenPolicy.SharedFolder(tag: "docs", hostRoot: URL(fileURLWithPath: "/tmp/docs"))]
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let destination = root.appendingPathComponent("saved.txt")
        let result = await coordinator(shares: { shares }, action: { _ in .save(destination) }, afterQuarantine: { shares = [] })
            .handle(.init(kind: .file, value: "/mnt/linportal/docs/file.txt"))
        XCTAssertEqual(result.status, .refused); XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: try XCTUnwrap(received.mounted.last).path))
    }
    func testCancellationWhileApprovalWaitsCannotOpenLater() async {
        var gate: CheckedContinuation<GuestOpenAction, Never>?
        let opening = coordinator(action: { _ in await withCheckedContinuation { gate = $0 } })
        let task = Task { await opening.handle(.init(kind: .url, value: "https://example.com")) }
        while gate == nil { await Task.yield() }
        task.cancel(); gate?.resume(returning: .open)
        let response = await task.value
        XCTAssertEqual(response.status, .refused); XCTAssertTrue(opened.isEmpty)
    }
    func testStopAndConcurrentRequestsCannotCreateExtraDialogs() async {
        var gate: CheckedContinuation<GuestOpenAction, Never>?
        let opening = coordinator(action: { _ in await withCheckedContinuation { gate = $0 } })
        let first = Task { await opening.handle(.init(kind: .url, value: "https://example.com")) }
        while gate == nil { await Task.yield() }
        let second = await opening.handle(.init(kind: .url, value: "https://example.com/second"))
        XCTAssertEqual(second.status, .refused); XCTAssertEqual(prompts.count, 1)
        opening.stop(); gate?.resume(returning: .open)
        let response = await first.value
        XCTAssertEqual(response.status, .refused); XCTAssertTrue(opened.isEmpty)
    }
    func testRealQuarantineMarkAppliesToFoldersAndChildren() async throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let child = root.appendingPathComponent("script.command")
        try received.contents.write(to: child)
        try await GuestOpenCoordinator.markAsDownloaded(root)
        for file in [root!, child] {
            XCTAssertEqual(try file.resourceValues(forKeys: [.quarantinePropertiesKey]).quarantineProperties?["LSQuarantineAgentName"] as? String, "NativePipe")
        }
    }
}
