import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class FilePromiseTests: XCTestCase {
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
        let promise = LinuxFilePromise(remote: URL(fileURLWithPath: "/guest/example.bin"), access: access)
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
    nonisolated static let contents = Data(repeating: 0x5a, count: 32 * 1024 * 1024)
    func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL] { urls }
    func exportFile(_ remote: URL, to local: URL) async throws {
        try await Task.sleep(for: .milliseconds(10))
        try Self.contents.write(to: local, options: .withoutOverwriting)
    }
}
