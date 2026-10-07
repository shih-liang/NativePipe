import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class FilePromiseTests: XCTestCase {
    func testMountedPromiseWaitsForAcceptanceThenUsesNativeCopyWithoutRemoteExport() throws {
        _ = NSApplication.shared
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("mounted-promise-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let source = directory.appendingPathComponent("source.txt"), destination = directory.appendingPathComponent("received.txt")
        try Data("mounted bytes".utf8).write(to: source)
        let access = PromiseFileAccess()
        var publications = 0
        let promise = LinuxFilePromise(remote: URL(fileURLWithPath: "/guest/source.txt"), access: access,
            publishGuestFiles: { files, _, purpose in
                XCTAssertEqual(files.map(\.path), ["/guest/source.txt"]); XCTAssertEqual(purpose, .drag)
                publications += 1
                return [source]
            })
        nonisolated(unsafe) let provider = promise.provider
        XCTAssertEqual(publications, 0)
        let received = expectation(description: "native mounted copy")
        OperationQueue().addOperation {
            promise.filePromiseProvider(provider, writePromiseTo: destination) { error in
                XCTAssertNil(error)
                XCTAssertEqual(try? Data(contentsOf: destination), Data("mounted bytes".utf8))
                received.fulfill()
            }
        }
        wait(for: [received], timeout: 5)
        XCTAssertEqual(publications, 1); XCTAssertEqual(access.exports, 0)
    }

    func testNativePublishedCopyRejectsReplacementAndCleansReadOnlyStaging() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("mounted-copy-" + UUID().uuidString)
        let source = directory.appendingPathComponent("source", isDirectory: true)
        let child = source.appendingPathComponent("child.txt")
        let destination = directory.appendingPathComponent("existing.txt")
        try FileManager.default.createDirectory(at: source, withIntermediateDirectories: true)
        try Data("source".utf8).write(to: child)
        try FileManager.default.setAttributes([.posixPermissions: 0o444], ofItemAtPath: child.path)
        try FileManager.default.setAttributes([.posixPermissions: 0o555], ofItemAtPath: source.path)
        defer {
            try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: source.path)
            try? FileManager.default.removeItem(at: directory)
        }
        try Data("keep".utf8).write(to: destination)
        do { try await FileTransferLocalIO.copy(source: source, to: destination); XCTFail("Existing destinations must survive") }
        catch { XCTAssertEqual((error as? POSIXError)?.code, .EEXIST) }
        XCTAssertEqual(try Data(contentsOf: destination), Data("keep".utf8))
        XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: directory.path).contains { $0.hasPrefix(".nativepipe-drag-") })
    }

    func testCancelledMountedPromiseNeverPublishesOrCreatesDestination() throws {
        _ = NSApplication.shared
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("cancelled-promise-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let destination = directory.appendingPathComponent("file")
        let promise = LinuxFilePromise(remote: URL(fileURLWithPath: "/guest/file"), access: PromiseFileAccess(),
            publishGuestFiles: { _, _, _ in XCTFail("Cancelled drop must not mount or read"); return [] })
        promise.cancel()
        nonisolated(unsafe) let provider = promise.provider
        let cancelled = expectation(description: "cancelled promise")
        OperationQueue().addOperation {
            promise.filePromiseProvider(provider, writePromiseTo: destination) { error in
                XCTAssertTrue(error is CancellationError)
                cancelled.fulfill()
            }
        }
        wait(for: [cancelled], timeout: 5)
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
    }

    func testMissingPublisherNeverFallsBackToRemoteExport() throws {
        _ = NSApplication.shared
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("unavailable-promise-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let destination = directory.appendingPathComponent("file"), access = PromiseFileAccess()
        let promise = LinuxFilePromise(remote: URL(fileURLWithPath: "/guest/file"), access: access)
        nonisolated(unsafe) let provider = promise.provider
        let failed = expectation(description: "unavailable backend")
        OperationQueue().addOperation {
            promise.filePromiseProvider(provider, writePromiseTo: destination) { error in
                XCTAssertTrue(error is GuestFileSharingError); failed.fulfill()
            }
        }
        wait(for: [failed], timeout: 5)
        XCTAssertEqual(access.exports, 0)
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
    }

    func testProviderOwnsWriterAfterSourceDisappearsWithoutRetainCycle() throws {
        _ = NSApplication.shared
        var access: PromiseFileAccess? = PromiseFileAccess()
        weak var weakAccess = access
        var promise: LinuxFilePromise? = LinuxFilePromise(remote: URL(fileURLWithPath: "/guest/folder", isDirectory: true), access: access!)
        weak var weakPromise = promise
        var provider: NSFilePromiseProvider? = promise!.provider
        XCTAssertEqual(provider?.fileType, "public.folder")
        XCTAssertTrue(provider === promise!.provider)
        access = nil
        promise = nil
        XCTAssertNotNil(weakAccess)
        XCTAssertNotNil(weakPromise)
        XCTAssertEqual(provider?.delegate?.filePromiseProvider(provider!, fileNameForType: "public.folder"), "folder")
        provider = nil
        XCTAssertNil(weakPromise)
        XCTAssertNil(weakAccess)
    }

    func testIncomingPromisesRegisterBeforeWaitingAndShareOneDestination() async throws {
        let first = TestPromiseReceiver("first.txt"), second = TestPromiseReceiver("second.txt")
        XCTAssertTrue(first.fileNames.isEmpty)
        let receipt = try IncomingFilePromises([first, second])
        XCTAssertEqual(first.destination, receipt.directory)
        XCTAssertEqual(second.destination, receipt.directory)
        first.deliver(); second.deliver()
        let files = try await receipt.files()
        XCTAssertEqual(files.map(\.lastPathComponent), ["first.txt", "second.txt"])
    }

    func testCancelledIncomingPromiseKeepsStagingUntilTheProviderFinishes() async throws {
        let receiver = TestPromiseReceiver("later.txt")
        var receipt: IncomingFilePromises? = try IncomingFilePromises([receiver])
        let directory = receipt!.directory
        let task = Task { [receipt = receipt!] in try await receipt.files() }
        task.cancel()
        _ = await task.result
        receipt = nil
        XCTAssertTrue(FileManager.default.fileExists(atPath: directory.path))
        receiver.deliver()
        XCTAssertFalse(FileManager.default.fileExists(atPath: directory.path))
    }

    func testPromiseWritesUnderExistingAppKitFileCoordination() throws {
        _ = NSApplication.shared
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("nativepipe-promise-test-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let access = PromiseFileAccess()
        let source = directory.appendingPathComponent("mounted.bin")
        try PromiseFileAccess.contents.write(to: source)
        let promise = LinuxFilePromise(remote: URL(fileURLWithPath: "/guest/example.bin"), access: access,
            publishGuestFiles: { _, _, _ in [source] })
        // AppKit calls this delegate on its worker queue too. The test only
        // passes the provider through; it never reads or mutates it there.
        nonisolated(unsafe) let provider = promise.provider
        let written = expectation(description: "AppKit receives the file")
        let completed = expectation(description: "Wayland source can finish")
        promise.completed = { error in
            XCTAssertNil(error)
            completed.fulfill()
        }
        let queue = OperationQueue()
        queue.addOperation {
            // NSFilePromiseProvider already holds this claim when it calls its
            // delegate. A second coordinator inside the delegate deadlocks.
            var error: NSError?
            NSFileCoordinator().coordinate(writingItemAt: directory.appendingPathComponent("example.bin"),
                options: .forReplacing, error: &error) { url in
                promise.filePromiseProvider(provider, writePromiseTo: url) { error in
                    XCTAssertNil(error)
                    XCTAssertEqual(try? Data(contentsOf: url), PromiseFileAccess.contents)
                    written.fulfill()
                }
            }
            XCTAssertNil(error)
        }
        wait(for: [written, completed], timeout: 5)
        XCTAssertEqual(access.exports, 0)
        withExtendedLifetime(promise) {}
    }
}

private final class TestPromiseReceiver: NSFilePromiseReceiver, @unchecked Sendable {
    let name: String
    var destination: URL?
    private var reader: ((URL, Error?) -> Void)?
    init(_ name: String) { self.name = name; super.init() }
    required init?(pasteboardPropertyList propertyList: Any, ofType type: NSPasteboard.PasteboardType) { nil }
    override var fileNames: [String] { destination == nil ? [] : [name] }
    override func receivePromisedFiles(atDestination destinationDir: URL, options: [AnyHashable: Any],
        operationQueue: OperationQueue, reader: @escaping (URL, Error?) -> Void) {
        destination = destinationDir
        self.reader = reader
    }
    func deliver() {
        let callback = reader
        reader = nil
        callback?(destination!.appendingPathComponent(name), nil)
    }
}

@MainActor
private final class PromiseFileAccess: UserFileAccess {
    var exports = 0
    nonisolated static let contents = Data(repeating: 0x5a, count: 32 * 1024 * 1024)
    func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL] { urls }
    func exportFile(_ remote: URL, to local: URL) async throws {
        exports += 1
        try await Task.sleep(for: .milliseconds(10))
        try Self.contents.write(to: local, options: .withoutOverwriting)
    }
}
