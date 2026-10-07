import Darwin
import Foundation
import NativePipeProtocol
import XCTest
@testable import NativePipeWindowing

@MainActor
private final class ReceivedFiles: UserFileAccess {
    var count = 0
    var directory = false
    var size = 6
    var contents = Data("hello!".utf8)
    var fails = false
    var afterTransfer: (() -> Void)?
    var latestProgress: (@Sendable (FileTransferProgress) -> Void)?
    func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL] { urls }
    func exportFile(_ remote: URL, to local: URL) async throws {
        count += 1
        if fails { throw FileRPC.Failure.protocolError }
        if directory {
            try FileManager.default.createDirectory(at: local, withIntermediateDirectories: false)
            try contents.write(to: local.appendingPathComponent("child.txt"))
        } else {
            try contents.write(to: local)
            let handle = try FileHandle(forWritingTo: local)
            defer { try? handle.close() }
            try handle.truncate(atOffset: UInt64(size))
        }
        afterTransfer?()
    }
    func exportFile(_ remote: URL, to local: URL, progress: @escaping @Sendable (FileTransferProgress) -> Void) async throws {
        latestProgress = progress
        try await exportFile(remote, to: local)
        progress(.init(bytesTransferred: UInt64(size), isComplete: true))
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
        markFails: Bool = false) -> GuestOpenCoordinator {
        GuestOpenCoordinator(machineName: "Test Linux", isEnabled: enabled, shares: shares,
            fileAccess: received, scratch: root,
            confirm: { [self] prompt in prompts.append(prompt); return await action(prompt) },
            open: { [self] url in opened.append(url); return true },
            quarantine: { [self] url in
                if markFails { throw FileRPC.Failure.protocolError }
                marked.append(url)
            }, transferProgress: { _ in })
    }

    func testNoURLIsOpenedWithoutApproval() async {
        let result = await coordinator(action: { _ in .cancel }).handle(.init(kind: .url, value: "https://user:password@example.com"))
        XCTAssertEqual(result.status, .refused); XCTAssertTrue(opened.isEmpty); XCTAssertEqual(prompts.count, 1)
    }
    func testCredentialURLAndMailAttachmentReachLaunchServicesOnlyAfterApproval() async {
        for value in ["https://user:password@example.com", "mailto:test@example.com?attach=/tmp/test.pdf", "ssh://host"] {
            let result = await coordinator().handle(.init(kind: .url, value: value))
            XCTAssertEqual(result.status, .opened); XCTAssertEqual(opened.last?.absoluteString, value)
        }
    }
    func testDisabledFeatureDoesNotPromptOrTransfer() async {
        let result = await coordinator(enabled: { false }).handle(.init(kind: .file, value: "/home/test/file"))
        XCTAssertEqual(result.status, .disabled); XCTAssertTrue(prompts.isEmpty); XCTAssertEqual(received.count, 0)
    }
    func testCancellingInitialFileApprovalReceivesNoBytes() async {
        let result = await coordinator(action: { _ in .cancel }).handle(.init(kind: .file, value: "/home/test/tool.pkg"))
        XCTAssertEqual(result.status, .refused); XCTAssertEqual(received.count, 0)
        XCTAssertTrue(opened.isEmpty); XCTAssertFalse(FileManager.default.fileExists(atPath: root.path))
    }
    func testLargeFileUsesTheSameStreamingInterfaceWithoutASizeCeiling() async throws {
        received.size = 64 * 1024 * 1024 + 1
        let result = await coordinator().handle(.init(kind: .file, value: "/home/test/large.dat"))
        XCTAssertEqual(result.status, .opened)
        let file = try XCTUnwrap(opened.first)
        XCTAssertEqual(try file.resourceValues(forKeys: [.fileSizeKey]).fileSize, received.size)
        XCTAssertEqual(marked, opened)
    }
    func testFoldersTransferAndOpenThroughTheSameInterface() async throws {
        received.directory = true
        let result = await coordinator().handle(.init(kind: .file, value: "/home/test/folder"))
        XCTAssertEqual(result.status, .opened)
        let file = try XCTUnwrap(opened.first)
        XCTAssertEqual(try Data(contentsOf: file.appendingPathComponent("child.txt")), received.contents)
        XCTAssertEqual(prompts.count, 1, "Ordinary directories do not need execution approval")
    }
    func testSavingAProgramTransfersItWithoutRunningIt() async throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let destination = root.appendingPathComponent("tool.command")
        let result = await coordinator(action: { _ in .save(destination) })
            .handle(.init(kind: .file, value: "/home/test/tool.command"))
        XCTAssertEqual(result.status, .opened); XCTAssertTrue(opened.isEmpty)
        XCTAssertEqual(try Data(contentsOf: destination), received.contents)
        XCTAssertEqual(prompts.count, 1)
    }
    func testOpeningAProgramNeedsItsOwnApprovalAfterReceivingIt() async throws {
        let result = await coordinator(action: { prompt in
            if case .execute = prompt { return .cancel }; return .open
        }).handle(.init(kind: .file, value: "/home/test/tool.command"))
        XCTAssertEqual(result.status, .refused); XCTAssertEqual(received.count, 1)
        XCTAssertEqual(prompts.count, 2); XCTAssertTrue(opened.isEmpty)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: root.path).count, 0)
    }
    func testRevokingShareDuringApprovalPreventsTransfer() async {
        var shares = [GuestOpenPolicy.SharedFolder(tag: "docs", hostRoot: URL(fileURLWithPath: "/tmp/docs"))]
        let result = await coordinator(shares: { shares }, action: { _ in shares = []; return .open })
            .handle(.init(kind: .file, value: "/mnt/linportal/docs/file.txt"))
        XCTAssertEqual(result.status, .refused); XCTAssertEqual(received.count, 0); XCTAssertTrue(opened.isEmpty)
    }
    func testRevokingShareDuringTransferPreventsOpeningAndDeletesCopy() async throws {
        var shares = [GuestOpenPolicy.SharedFolder(tag: "docs", hostRoot: URL(fileURLWithPath: "/tmp/docs"))]
        received.afterTransfer = { shares = [] }
        let result = await coordinator(shares: { shares }).handle(.init(kind: .file, value: "/mnt/linportal/docs/file.txt"))
        XCTAssertEqual(result.status, .refused); XCTAssertTrue(opened.isEmpty)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: root.path).count, 0)
    }
    func testTransferOrQuarantineFailureNeverOpensAnything() async {
        received.fails = true
        let failed = await coordinator().handle(.init(kind: .file, value: "/home/test/file.txt"))
        XCTAssertEqual(failed.status, .failed)
        received.fails = false
        let unmarked = await coordinator(markFails: true).handle(.init(kind: .file, value: "/home/test/file.txt"))
        XCTAssertEqual(unmarked.status, .failed); XCTAssertTrue(opened.isEmpty)
    }
    func testCancellationWhileApprovalWaitsCannotOpenLater() async {
        var gate: CheckedContinuation<GuestOpenAction, Never>?
        let opening = coordinator(action: { _ in await withCheckedContinuation { gate = $0 } })
        let task = Task { await opening.handle(.init(kind: .url, value: "https://example.com")) }
        while gate == nil { await Task.yield() }
        task.cancel()
        gate?.resume(returning: .open)
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
    func testCleanupSkipsAnActiveApprovalThenCollectsACompletedCopy() async throws {
        var gate: CheckedContinuation<GuestOpenAction, Never>?
        let opening = coordinator(action: { prompt in
            if case .execute = prompt { return await withCheckedContinuation { gate = $0 } }
            return .open
        })
        let pending = Task { await opening.handle(.init(kind: .file, value: "/home/test/tool.command")) }
        while gate == nil { await Task.yield() }
        let directory = try XCTUnwrap(FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil).first)
        try FileManager.default.setAttributes([.modificationDate: Date.distantPast], ofItemAtPath: directory.path)
        let anotherOwner = coordinator()
        await anotherOwner.removeOldCopies(olderThan: 0)
        XCTAssertTrue(FileManager.default.fileExists(atPath: directory.path), "Active copies hold a cross-owner lease")
        gate?.resume(returning: .open)
        let result = await pending.value
        XCTAssertEqual(result.status, .opened)
        await anotherOwner.removeOldCopies(olderThan: 0)
        XCTAssertFalse(FileManager.default.fileExists(atPath: directory.path))
    }
    func testLateProgressFromARevokedShareCannotCancelTheNextRequest() async throws {
        var shares = [GuestOpenPolicy.SharedFolder(tag: "docs", hostRoot: URL(fileURLWithPath: "/tmp/docs"))]
        var gate: CheckedContinuation<GuestOpenAction, Never>?
        let opening = coordinator(shares: { shares }, action: { prompt in
            if case .link = prompt { return await withCheckedContinuation { gate = $0 } }
            return .open
        })
        let first = await opening.handle(.init(kind: .file, value: "/mnt/linportal/docs/file.txt"))
        XCTAssertEqual(first.status, .opened)
        let lateProgress = try XCTUnwrap(received.latestProgress)
        try await Task.sleep(for: .milliseconds(1010))
        shares = []
        let next = Task { await opening.handle(.init(kind: .url, value: "https://example.com")) }
        while gate == nil { await Task.yield() }
        lateProgress(.init(bytesTransferred: 1))
        await Task.yield(); await Task.yield()
        gate?.resume(returning: .open)
        let response = await next.value
        XCTAssertEqual(response.status, .opened)
        XCTAssertEqual(opened.last?.absoluteString, "https://example.com")
    }
}
