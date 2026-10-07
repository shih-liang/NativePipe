import Darwin
import Foundation
import NativePipeProtocol
import XCTest
@testable import NativePipeFileSharing
@testable import NativePipeFileSystem

/// A real AF_UNIX broker fixture tests the native volume's range adapter without
/// pretending to enable or mount an FSKit extension in a unit-test process.
@MainActor
@available(macOS 27.0, *)
final class SharedFilesRangeTests: XCTestCase {
    override func setUpWithError() throws {
        try super.setUpWithError()
        guard ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27 else {
            throw XCTSkip("Native FSKit sharing requires macOS 27 or later")
        }
    }

    private actor Reads {
        var ranges: [(UInt64, Int)] = []
        func response(_ request: SharedFileRequest) -> SharedFileReply {
            guard case .read(_, let offset, let length, _) = request.operation else {
                return SharedFileReply(error: EINVAL)
            }
            ranges.append((offset, length))
            return SharedFileReply(data: Data((0..<length).map { UInt8(truncatingIfNeeded: offset + UInt64($0)) }))
        }
        func snapshot() -> [(UInt64, Int)] { ranges }
    }

    private func fixture() throws -> (URL, LocalSocketServer, SharedFilesVolume, SharedFileItem, Reads) {
        let directory = URL(fileURLWithPath: "/tmp/fsk-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])
        let manifest = directory.appendingPathComponent("volume.json")
        try JSONEncoder().encode(SharedFileVolumeDescriptor(id: UUID(), name: "Shared Files", token: UUID()))
            .write(to: manifest)
        guard chmod(manifest.path, 0o600) == 0 else { throw POSIXError(.EACCES) }
        let reads = Reads()
        let server = LocalSocketServer(url: directory.appendingPathComponent("files.sock"))
        try server.start { connection in
            do {
                let request = try JSONDecoder().decode(SharedFileRequest.self,
                    from: await FileRPC.receiveRecord(from: connection, maximum: 16 * 1024, deadline: .now() + .seconds(5)))
                let response = await reads.response(request)
                try await FileRPC.sendRecord(JSONEncoder().encode(response), to: connection, deadline: .now() + .seconds(5))
            } catch { connection.close() }
        }
        let volume = SharedFilesVolume(client: try SharedFileVolumeClient(directory: directory))
        let file = SharedFileItem(id: 3, parentID: 2, name: "large.bin", kind: .file,
            size: 5 * 1024 * 1024 * 1024, modified: nil, permissions: 0o644, version: Data([1]))
        return (directory, server, volume, file, reads)
    }

    func testLargeKernelRequestStreamsOnlyRequestedRanges() async throws {
        let (directory, server, volume, file, reads) = try fixture()
        defer { server.stop(); try? FileManager.default.removeItem(at: directory) }
        let initial = await reads.snapshot()
        XCTAssertTrue(initial.isEmpty)
        let length = 2 * SharedFileVolumeClient.maximumReadLength + 37
        var copied = 0
        let completed = try await volume.readRange(file, offset: 123, length: length) { data, position in
            XCTAssertEqual(position, copied)
            XCTAssertEqual(data.first, UInt8(truncatingIfNeeded: 123 + position))
            XCTAssertEqual(data.last, UInt8(truncatingIfNeeded: 123 + position + data.count - 1))
            copied += data.count
        }
        XCTAssertEqual(completed, length)
        XCTAssertEqual(copied, length)
        let ranges = await reads.snapshot()
        XCTAssertEqual(ranges.map { $0.0 }, [123, UInt64(123 + SharedFileVolumeClient.maximumReadLength), UInt64(123 + 2 * SharedFileVolumeClient.maximumReadLength)])
        XCTAssertEqual(ranges.map { $0.1 }, [SharedFileVolumeClient.maximumReadLength, SharedFileVolumeClient.maximumReadLength, 37])
    }

    func testCancellationStopsBeforeFetchingAnotherRange() async throws {
        let (directory, server, volume, file, reads) = try fixture()
        defer { server.stop(); try? FileManager.default.removeItem(at: directory) }
        let task = Task {
            try await volume.readRange(file, offset: 0, length: 2 * SharedFileVolumeClient.maximumReadLength) { _, _ in
                withUnsafeCurrentTask { $0?.cancel() }
            }
        }
        do { _ = try await task.value; XCTFail("cancelled range read succeeded") }
        catch is CancellationError { }
        let ranges = await reads.snapshot()
        XCTAssertEqual(ranges.count, 1)
    }
}
