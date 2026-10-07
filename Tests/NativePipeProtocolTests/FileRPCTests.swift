import XCTest
import Darwin
import CNativePipeFileRPC

private final class FileProgressRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var recorded: [FileTransferProgress] = []
    var values: [FileTransferProgress] { lock.lock(); defer { lock.unlock() }; return recorded }
    func append(_ sample: FileTransferProgress) { lock.lock(); recorded.append(sample); lock.unlock() }
}
@testable import NativePipeProtocol

final class FileRPCTests: XCTestCase {
    func testRemoteFileURLsPreserveByteDistinctLinuxUnicodeNamesAndPunctuation() throws {
        let names = ["\u{e9}.txt", "e\u{301}.txt", "a #?% \n.txt"]
        let directory = try RemoteFileURL.make("/guest/\u{e9}", isDirectory: true)
        let urls = try names.map { try RemoteFileURL.appending($0, to: directory) }
        for (name, url) in zip(names, urls) {
            XCTAssertEqual(Data(url.path.utf8), Data(("/guest/\u{e9}/" + name).utf8))
            XCTAssertEqual(Data(try FileTransferURLs.decode(FileTransferURLs.encode([url]))[0].path.utf8), Data(url.path.utf8))
        }
        XCTAssertNotEqual(Data(urls[0].path.utf8), Data(urls[1].path.utf8))
        XCTAssertThrowsError(try RemoteFileURL.make("relative"))
        XCTAssertThrowsError(try RemoteFileURL.appending("../escape", to: directory))
        XCTAssertThrowsError(try RemoteFileURL.appending("file", to: XCTUnwrap(URL(string: "file://foreign/guest"))))
    }

    func testSnapshotPreservesTheRequestedLinuxPathBytesInItsMetadataURL() async throws {
        let path = "/guest/\u{e9}.txt"
        var descriptors: [Int32] = [-1, -1]
        XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors), 0)
        let peer = descriptors[0], client = FileHandle(fileDescriptor: descriptors[1], closeOnDealloc: true)
        let finished = DispatchGroup(); finished.enter()
        DispatchQueue.global().async {
            defer { close(peer); finished.leave() }
            let frame = UnsafeMutablePointer<np_file_frame>.allocate(capacity: 1)
            defer { frame.deallocate() }
            guard np_file_receive(peer, frame) == 0 else { return }
            XCTAssertEqual(frame.pointee.type, UInt8(NP_FILE_SNAPSHOT.rawValue))
            let payload = Data(bytes: np_file_frame_data(frame), count: Int(frame.pointee.length))
            XCTAssertEqual(payload.dropFirst(4), Data(path.utf8))
            var metadata = [UInt8](repeating: 0, count: 28 + Int(NP_FILE_REVISION))
            np_file_put32(&metadata, 0o100600)
            _ = np_file_send(peer, UInt8(NP_FILE_METADATA.rawValue), 0, 0, &metadata, metadata.count)
        }
        let info = try await FileRPC { client }.snapshot(path)
        XCTAssertEqual(Data(info.url.path.utf8), Data(path.utf8))
        XCTAssertEqual(finished.wait(timeout: .now() + 2), .success)
    }
    func testAsyncRecordDecoderRejectsMalformedAndTruncatedFrames() async throws {
        func header(type: UInt8 = UInt8(NP_FILE_DATA.rawValue), length: UInt32, flags: UInt8 = 0) -> Data {
            var bytes = Data([78, 80, 70, 82, UInt8(NP_FILE_VERSION), type, flags, 0])
            var length = length.littleEndian
            withUnsafeBytes(of: &length) { bytes.append(contentsOf: $0) }
            bytes.append(Data(repeating: 0, count: 4))
            return bytes
        }
        let invalid = [
            header(length: 1, flags: 1) + Data([0]),
            header(type: 255, length: 1) + Data([0]),
            header(length: UInt32(NP_FILE_CHUNK) + 1),
            header(length: 9) + Data(repeating: 0, count: 9), // Record limit is 8.
            header(type: UInt8(NP_FILE_END.rawValue), length: 8) + Data([1, 0, 0, 0, 0, 0, 0, 0]),
            header(length: 1), // EOF in payload.
            Data([78, 80, 70]) // EOF in header.
        ]
        for bytes in invalid {
            var descriptors: [Int32] = [-1, -1]
            XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors), 0)
            let receiver = try SocketConnection(owning: descriptors[0])
            let sender = try SocketConnection(owning: descriptors[1])
            defer { receiver.close(); sender.close() }
            try await sender.write(bytes)
            sender.finishWriting()
            do {
                _ = try await FileRPC.receiveRecord(from: receiver, maximum: 8, deadline: .now() + .seconds(1))
                XCTFail("Malformed record was accepted")
            } catch is FileRPC.Failure {
            } catch is SocketConnection.Failure {
            } catch { XCTFail("Unexpected failure: \(error)") }
        }
    }

    private final class Server: @unchecked Sendable {
        let root: URL
        let workers = DispatchGroup()
        init() throws {
            root = FileManager.default.temporaryDirectory.appendingPathComponent("file-rpc-\(UUID())")
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
        }
        deinit { try? FileManager.default.removeItem(at: root) }
        func connect() throws -> FileHandle {
            var fds: [Int32] = [-1, -1]
            guard socketpair(AF_UNIX, SOCK_STREAM, 0, &fds) == 0 else { throw POSIXError(.EIO) }
            let server = fds[0], client = fds[1]
            workers.enter()
            DispatchQueue.global().async { [self] in
                defer { close(server); workers.leave() }
                let fd = open(root.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
                guard fd >= 0 else { return }
                defer { close(fd) }
                _ = np_file_serve(server, fd, 0)
            }
            return FileHandle(fileDescriptor: client, closeOnDealloc: true)
        }
        var rpc: FileRPC { FileRPC { [self] in try connect() } }
    }

    func testSwiftClientWithSharedCServer() async throws {
        let server = try Server()
        let sourceURL = server.root.appendingPathComponent("original")
        let destinationURL = server.root.appendingPathComponent("received")
        let bytes = Data(repeating: 0x93, count: 32 * 1024 * 1024 + 13)
        try bytes.write(to: sourceURL)
        let source = try FileHandle(forReadingFrom: sourceURL)
        defer { try? source.close() }
        try await server.rpc.upload(source, to: "/remote", mode: 0o640)
        let info = try await server.rpc.stat("/remote")
        XCTAssertEqual(info.size, UInt64(bytes.count)); XCTAssertEqual(info.permissions, 0o640)
        XCTAssertTrue(FileManager.default.createFile(atPath: destinationURL.path, contents: nil))
        let destination = try FileHandle(forWritingTo: destinationURL)
        try await server.rpc.download("/remote", to: destination)
        try destination.close()
        XCTAssertEqual(try Data(contentsOf: destinationURL), bytes)
        try await server.rpc.write(Data(), to: "/empty", replace: false)
        let empty = try await server.rpc.read("/empty")
        XCTAssertTrue(empty.data.isEmpty)
        let list = try await server.rpc.read("/")
        XCTAssertEqual(Set(list.entries.map(\.name)), ["original", "received", "remote", "empty"])
        do { _ = try await server.rpc.read("/remote"); XCTFail("large reads need streaming") }
        catch FileRPC.Failure.tooLarge {}
        XCTAssertEqual(server.workers.wait(timeout: .now() + 2), .success)
    }

    func testCancellationWakesBlockedRead() async throws {
        var fds: [Int32] = [-1, -1]
        XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &fds), 0)
        let client = FileHandle(fileDescriptor: fds[0], closeOnDealloc: true)
        let peer = fds[1]
        defer { close(peer) }
        let task = Task { try await FileRPC { client }.stat("/waiting") }
        try await Task.sleep(for: .milliseconds(30))
        task.cancel()
        do { _ = try await task.value; XCTFail("cancelled operation succeeded") }
        catch is CancellationError {}
    }

    @MainActor
    func testClosingRangeAccessCancelsAnActiveVMRead() async throws {
        var descriptors: [Int32] = [-1, -1]
        XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors), 0)
        let client = FileHandle(fileDescriptor: descriptors[0], closeOnDealloc: true), peer = descriptors[1]
        defer { close(peer) }
        let requestArrived = expectation(description: "Range request reached the server")
        DispatchQueue.global().async {
            let frame = UnsafeMutablePointer<np_file_frame>.allocate(capacity: 1)
            defer { frame.deallocate() }
            if np_file_receive(peer, frame) == 0, frame.pointee.type == UInt8(NP_FILE_RANGE.rawValue) { requestArrived.fulfill() }
        }
        let access = FileRPCUserAccess(rpc: FileRPC { client })
        let task = Task { try await access.read(URL(fileURLWithPath: "/waiting"), offset: 0, length: 1,
                                               expectedVersion: Data(repeating: 0, count: Int(NP_FILE_REVISION))) }
        await fulfillment(of: [requestArrived], timeout: 2)
        let started = ProcessInfo.processInfo.systemUptime
        access.closeRangeAccess()
        do { _ = try await task.value; XCTFail("Revoked VM range succeeded") }
        catch is CancellationError { }
        XCTAssertLessThan(ProcessInfo.processInfo.systemUptime - started, 0.5)
    }

    @MainActor
    func testLazyRangeAccessReadsOnlyRequestedBytesAndRejectsChangedSources() async throws {
        let server = try Server(), source = server.root.appendingPathComponent("large")
        let file = open(source.path, O_RDWR | O_CREAT | O_EXCL, 0o751)
        XCTAssertGreaterThanOrEqual(file, 0); defer { close(file) }
        let offset: UInt64 = 1 << 40
        let contents = Array("requested-bytes".utf8)
        XCTAssertEqual(contents.withUnsafeBytes { pwrite(file, $0.baseAddress, $0.count, off_t(offset)) }, contents.count)
        let access: any UserFileRangeAccess = FileRPCUserAccess(rpc: server.rpc)
        let remote = URL(fileURLWithPath: "/large")
        let info = try await access.metadata(for: remote)
        XCTAssertEqual(info.size, offset + UInt64(contents.count)); XCTAssertEqual(info.kind, .file)
        XCTAssertEqual(info.permissions, 0o751)
        XCTAssertEqual(try JSONDecoder().decode(UserFileMetadata.self, from: JSONEncoder().encode(info)), info)
        let bytes = try await access.read(remote, offset: offset + 2, length: 4, expectedVersion: info.version)
        XCTAssertEqual(bytes, Data("ques".utf8))
        let tail = try await access.read(remote, offset: info.size - 2, length: 10, expectedVersion: info.version)
        XCTAssertEqual(tail, Data("es".utf8))
        let eof = try await access.read(remote, offset: info.size, length: 10, expectedVersion: info.version)
        XCTAssertTrue(eof.isEmpty)
        let empty = try await access.read(remote, offset: 0, length: 0, expectedVersion: info.version)
        XCTAssertTrue(empty.isEmpty)
        for (position, length) in [(UInt64.max, 1), (0, UserFileRange.maximumReadLength + 1), (0, -1)] {
            do { _ = try await access.read(remote, offset: position, length: length, expectedVersion: info.version); XCTFail("Invalid range") }
            catch FileRPC.Failure.invalidPath {}
        }
        let listing = try await access.contents(of: URL(fileURLWithPath: "/"))
        XCTAssertEqual(listing.map(\.url.lastPathComponent), ["large"])
        XCTAssertEqual(contents.withUnsafeBytes { pwrite(file, $0.baseAddress, 1, off_t(offset)) }, 1)
        do { _ = try await access.read(remote, offset: offset, length: 1, expectedVersion: info.version); XCTFail("Stale source accepted") }
        catch FileRPC.Failure.sourceChanged(let path) { XCTAssertEqual(path, "/large") }
        XCTAssertEqual(server.workers.wait(timeout: .now() + 2), .success)
    }

    @MainActor
    func testLazyRangeAccessNeverFollowsSelectedFolderSymlinks() async throws {
        let server = try Server()
        try FileManager.default.createDirectory(at: server.root.appendingPathComponent("folder"), withIntermediateDirectories: false)
        try Data("contents".utf8).write(to: server.root.appendingPathComponent("folder/file"))
        try FileManager.default.createSymbolicLink(atPath: server.root.appendingPathComponent("link").path, withDestinationPath: "folder")
        try FileManager.default.createSymbolicLink(atPath: server.root.appendingPathComponent("folder/file-link").path, withDestinationPath: "file")
        let access = FileRPCUserAccess(rpc: server.rpc)
        let listing = try await access.contents(of: URL(fileURLWithPath: "/folder"))
        XCTAssertEqual(listing.map(\.url.lastPathComponent), ["file"])
        for path in ["/link/file", "/folder/file-link", "/folder/../folder/file"] {
            do { _ = try await access.metadata(for: URL(fileURLWithPath: path)); XCTFail("Lazy access followed unsafe path: \(path)") }
            catch { }
        }
        do { _ = try await access.contents(of: URL(fileURLWithPath: "/link")); XCTFail("Linked folder was listed") }
        catch { }
        XCTAssertEqual(server.workers.wait(timeout: .now() + 2), .success)
    }

    @MainActor
    func testUserFilesUseStreamingAndNeverMergeIntoExistingDestination() async throws {
        let server = try Server()
        try await server.rpc.createDirectory("/tmp")
        let files = FileRPCUserAccess(rpc: server.rpc)
        let source = server.root.appendingPathComponent("folder")
        try FileManager.default.createDirectory(at: source, withIntermediateDirectories: false)
        let bytes = Data(repeating: 0xa5, count: 32 * 1024 * 1024 + 13)
        try bytes.write(to: source.appendingPathComponent("hello # world.txt"))
        try Data().write(to: source.appendingPathComponent("empty.txt"))
        let uploadProgress = FileProgressRecorder()
        let paths = try await files.importFiles([source], shareDirectories: true,
                                               progress: { uploadProgress.append($0) })
        XCTAssertTrue(paths[0].path.hasPrefix("/tmp/nativepipe-drop-"))
        XCTAssertTrue(uploadProgress.values.contains { $0.relativePath == "folder/empty.txt" })
        let received = server.root.appendingPathComponent("result")
        let progress = FileProgressRecorder()
        try await files.exportFile(paths[0], to: received, progress: { progress.append($0) })
        XCTAssertEqual(try Data(contentsOf: received.appendingPathComponent("hello # world.txt")), bytes)
        XCTAssertEqual(try Data(contentsOf: received.appendingPathComponent("empty.txt")).count, 0)
        XCTAssertEqual(progress.values.last?.bytesTransferred, UInt64(bytes.count))
        XCTAssertEqual(progress.values.last?.totalBytes, UInt64(bytes.count))
        XCTAssertEqual(progress.values.last?.isComplete, true)
        XCTAssertTrue(progress.values.contains { $0.relativePath == "hello # world.txt" })
        XCTAssertTrue(progress.values.contains { $0.relativePath == "empty.txt" })
        XCTAssertEqual(progress.values.map(\.bytesTransferred), progress.values.map(\.bytesTransferred).sorted())
        do { try await files.exportFile(paths[0], to: received); XCTFail("must not merge") } catch { }
        XCTAssertEqual(try FileTransferURLs.decode(FileTransferURLs.encode(paths)), paths)
        for invalid in ["file:///", "file://foreign/tmp/file", "https://example.com/file", "file:///tmp/file%00"] {
            XCTAssertThrowsError(try FileTransferURLs.decode(Data(invalid.utf8)))
        }
        XCTAssertTrue(FileRPC.Failure.remote(38).localizedDescription.contains("not implemented"))
        XCTAssertTrue(FileRPC.Failure.remote(40).localizedDescription.contains("symbolic"))
        XCTAssertTrue(FileRPC.Failure.remote(104).localizedDescription.contains("reset"))
    }

    @MainActor
    func testFailedTreeExportPublishesNothingAndRemovesStaging() async throws {
        let server = try Server()
        let folder = server.root.appendingPathComponent("source")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)
        try Data("downloaded before failure".utf8).write(to: folder.appendingPathComponent("file"))
        try FileManager.default.createSymbolicLink(at: folder.appendingPathComponent("link"),
                                                 withDestinationURL: server.root.appendingPathComponent("outside"))
        let destination = server.root.appendingPathComponent("destination")
        let progress = FileProgressRecorder()
        do {
            try await FileRPCUserAccess(rpc: server.rpc).exportFile(URL(fileURLWithPath: "/source"),
                                                                  to: destination, progress: { progress.append($0) })
            XCTFail("A symbolic link must not be exported")
        } catch { }
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
        XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: server.root.path)
            .contains { $0.hasPrefix(".nativepipe-transfer-") })
        XCTAssertFalse(progress.values.contains { $0.isComplete })
    }

    @MainActor
    func testMultiSelectionExportUsesTheSameStreamingEngine() async throws {
        let server = try Server()
        try Data("first".utf8).write(to: server.root.appendingPathComponent("first"))
        try Data("second".utf8).write(to: server.root.appendingPathComponent("second"))
        let destination = server.root.appendingPathComponent("downloads")
        try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: false)
        let progress = FileProgressRecorder()
        let exported = try await FileRPCUserAccess(rpc: server.rpc).exportFiles(
            [URL(fileURLWithPath: "/first"), URL(fileURLWithPath: "/second")],
            to: destination, progress: { progress.append($0) })
        XCTAssertEqual(exported.map(\.lastPathComponent), ["first", "second"])
        XCTAssertEqual(try String(contentsOf: exported[0], encoding: .utf8), "first")
        XCTAssertEqual(try String(contentsOf: exported[1], encoding: .utf8), "second")
        XCTAssertEqual(progress.values.last?.bytesTransferred, 11)
        XCTAssertEqual(progress.values.last?.totalBytes, 11)
        XCTAssertEqual(progress.values.filter(\.isComplete).count, 1)
    }

    func testBootstrapRequiresFramedPayload() throws {
        let request = AgentWire.Request(name: "nativepipe-guestd")
        var encoded = try AgentWire.encodeRequest(request)
        XCTAssertEqual(try AgentWire.decodeRequest(from: encoded), request)
        encoded[5] = 0
        XCTAssertThrowsError(try AgentWire.decodeRequest(from: encoded))
    }

    func testUploadStopsWhenReceiverFailsBeforeEOF() async throws {
        let server = try Server()
        let sourceURL = server.root.appendingPathComponent("upload")
        let size = 10 * 1024 * 1024
        try Data(repeating: 0x72, count: size).write(to: sourceURL)
        let source = try FileHandle(forReadingFrom: sourceURL)
        defer { try? source.close() }
        var fds: [Int32] = [-1, -1]
        XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &fds), 0)
        let peer = fds[0], client = FileHandle(fileDescriptor: fds[1], closeOnDealloc: true)
        let finished = DispatchGroup()
        finished.enter()
        DispatchQueue.global().async {
            defer { close(peer); finished.leave() }
            let frame = UnsafeMutablePointer<np_file_frame>.allocate(capacity: 1)
            defer { frame.deallocate() }
            guard np_file_receive(peer, frame) == 0 else { return }
            var metadata = [UInt8](repeating: 0, count: 28)
            _ = np_file_send(peer, UInt8(NP_FILE_METADATA.rawValue), 0, 0, &metadata, metadata.count)
            guard np_file_receive(peer, frame) == 0 else { return }
            _ = np_file_send(peer, UInt8(NP_FILE_END.rawValue), 0, UInt32(ENOSPC), nil, 0)
            shutdown(peer, SHUT_WR)
            var buffer = [UInt8](repeating: 0, count: 65536)
            while read(peer, &buffer, buffer.count) > 0 {}
        }
        do {
            try await FileRPC { client }.upload(source, to: "/destination")
            XCTFail("Receiver failure was ignored")
        } catch FileRPC.Failure.remote(let code) {
            XCTAssertEqual(code, ENOSPC)
        }
        XCTAssertLessThan(try source.offset(), UInt64(size))
        XCTAssertEqual(finished.wait(timeout: .now() + 2), .success)
    }
}
