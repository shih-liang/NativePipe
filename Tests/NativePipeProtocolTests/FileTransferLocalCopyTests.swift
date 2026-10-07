import Darwin
import Foundation
import XCTest
@testable import NativePipeProtocol

private final class CopyProgressSamples: @unchecked Sendable {
    private let lock = NSLock()
    private var values: [FileTransferProgress] = []
    var samples: [FileTransferProgress] { lock.withLock { values } }
    func append(_ sample: FileTransferProgress) { lock.withLock { values.append(sample) } }
}

@MainActor
final class FileTransferLocalCopyTests: XCTestCase {
    func testFolderNativeCopyReportsActualAggregateBytesAndPreservesSource() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("native-copy-tree-" + UUID().uuidString)
        let source = root.appendingPathComponent("source", isDirectory: true)
        try FileManager.default.createDirectory(at: source, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        try Data("abc".utf8).write(to: source.appendingPathComponent("first.txt"))
        let child = source.appendingPathComponent("child", isDirectory: true)
        try FileManager.default.createDirectory(at: child, withIntermediateDirectories: false)
        try Data("12345".utf8).write(to: child.appendingPathComponent("second.txt"))
        let samples = CopyProgressSamples(), destination = root.appendingPathComponent("copied", isDirectory: true)
        try await FileTransferLocalIO.copy(source: source, to: destination, progress: { samples.append($0) })
        XCTAssertEqual(try Data(contentsOf: destination.appendingPathComponent("first.txt")), Data("abc".utf8))
        XCTAssertEqual(try Data(contentsOf: destination.appendingPathComponent("child/second.txt")), Data("12345".utf8))
        let completion = try XCTUnwrap(samples.samples.last)
        XCTAssertTrue(completion.isComplete); XCTAssertEqual(completion.bytesTransferred, 8); XCTAssertEqual(completion.totalBytes, 8)
        XCTAssertEqual(samples.samples.filter(\.isComplete).count, 1)
        XCTAssertTrue(samples.samples.contains { $0.relativePath == "child/second.txt" })
        XCTAssertEqual(try Data(contentsOf: source.appendingPathComponent("first.txt")), Data("abc".utf8))
    }

    func testCancellationDuringNativeCopyRemovesOnlyUnpublishedStage() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("native-copy-cancel-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source"), destination = root.appendingPathComponent("destination")
        try Data(repeating: 0x5a, count: 4 * 1024 * 1024).write(to: source)
        let samples = CopyProgressSamples(), unblock = DispatchSemaphore(value: 0)
        let copy = Task {
            try await FileTransferLocalIO.copy(source: source, to: destination) { sample in
                if samples.samples.isEmpty {
                    samples.append(sample)
                    _ = unblock.wait(timeout: .now() + 5)
                }
            }
        }
        for _ in 0..<100 {
            if !samples.samples.isEmpty { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertFalse(samples.samples.isEmpty)
        copy.cancel(); unblock.signal()
        do { try await copy.value; XCTFail("Cancelled copy must fail") }
        catch { XCTAssertTrue(error is CancellationError) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: root.path), ["source"])
        XCTAssertEqual(try source.resourceValues(forKeys: [.fileSizeKey]).fileSize, 4 * 1024 * 1024)
    }

    func testNativeCopyNeverBlocksOwnerMainActor() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("native-copy-owner-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source"), destination = root.appendingPathComponent("destination")
        try Data("bytes".utf8).write(to: source)
        let samples = CopyProgressSamples(), responded = DispatchSemaphore(value: 0)
        try await FileTransferLocalIO.copy(source: source, to: destination) { sample in
            samples.append(sample)
            if !sample.isComplete {
                Task { @MainActor in responded.signal() }
                XCTAssertEqual(responded.wait(timeout: .now() + 2), .success)
            }
        }
        XCTAssertEqual(try Data(contentsOf: destination), Data("bytes".utf8))
        XCTAssertTrue(try XCTUnwrap(samples.samples.last).isComplete)
    }
}
