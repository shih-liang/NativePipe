import XCTest
import Darwin
import CNativePipeFileRPC

private final class FileProgressRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var recorded: [FileTransferProgress] = []
    var values: [FileTransferProgress] { lock.lock(); defer { lock.unlock() }; return recorded }
    func append(_ sample: FileTransferProgress) { lock.lock(); recorded.append(sample); lock.unlock() }
}
private final class UploadTaskCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var task: Task<Void, Error>?
    func set(_ task: Task<Void, Error>) { lock.lock(); self.task = task; lock.unlock() }
    func cancel() { lock.lock(); let task = task; lock.unlock(); task?.cancel() }
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
        let readOnly: Bool
        private let countLock = NSLock()
        private var count = 0
        var connections: Int { countLock.lock(); defer { countLock.unlock() }; return count }
        init(readOnly: Bool = false) throws {
            self.readOnly = readOnly
            root = FileManager.default.temporaryDirectory.appendingPathComponent("file-rpc-\(UUID())")
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
        }
        deinit { try? FileManager.default.removeItem(at: root) }
        func connect() throws -> FileHandle {
            countLock.lock(); count += 1; countLock.unlock()
            var fds: [Int32] = [-1, -1]
            guard socketpair(AF_UNIX, SOCK_STREAM, 0, &fds) == 0 else { throw POSIXError(.EIO) }
            let server = fds[0], client = fds[1]
            workers.enter()
            DispatchQueue.global().async { [self] in
                defer { close(server); workers.leave() }
                let fd = open(root.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
                guard fd >= 0 else { return }
                defer { close(fd) }
                _ = np_file_serve(server, fd, readOnly ? 1 : 0)
            }
            return FileHandle(fileDescriptor: client, closeOnDealloc: true)
        }
        var rpc: FileRPC { FileRPC { [self] in try connect() } }
    }


    func testDirectoryBrowserReturnsAllMetadataInOneConnectionWithoutFollowingLinks() async throws {
        let server = try Server()
        let folder = server.root.appendingPathComponent("many")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)
        for index in 0..<600 {
            let name = String(repeating: "x", count: 200) + String(format: "%04d", index)
            try Data("metadata".utf8).write(to: folder.appendingPathComponent(name))
        }
        try FileManager.default.createDirectory(at: folder.appendingPathComponent("empty"), withIntermediateDirectories: false)
        try FileManager.default.createSymbolicLink(at: folder.appendingPathComponent("link"), withDestinationURL: URL(fileURLWithPath: "/etc/passwd"))
        XCTAssertEqual(mkfifo(folder.appendingPathComponent("fifo").path, 0o600), 0)
        let count = server.connections
        let listing = try await server.rpc.browse("/many")
        XCTAssertEqual(server.connections, count + 1)
        XCTAssertEqual(listing.path, "/many")
        XCTAssertTrue(listing.home.hasPrefix("/"))
        XCTAssertEqual(listing.entries.count, 603)
        for entry in listing.entries {
            var expected = stat()
            XCTAssertEqual(lstat(folder.appendingPathComponent(entry.name).path, &expected), 0)
            XCTAssertEqual(entry.metadata.mode, UInt32(expected.st_mode))
            XCTAssertEqual(entry.metadata.size, UInt64(expected.st_size))
            XCTAssertEqual(entry.metadata.mtime, Int64(expected.st_mtimespec.tv_sec))
            XCTAssertEqual(entry.metadata.path, "/many/" + entry.name)
        }
        let link = try XCTUnwrap(listing.entries.first(where: { $0.name == "link" }))
        XCTAssertTrue(link.metadata.isSymlink)
        do { _ = try await server.rpc.browse("/many/link"); XCTFail("Must not follow a symbolic link") } catch { }
        try FileManager.default.createSymbolicLink(at: server.root.appendingPathComponent("parent-link"), withDestinationURL: folder)
        do { _ = try await server.rpc.browse("/parent-link/empty"); XCTFail("Must not follow an intermediate link") } catch { }
        XCTAssertEqual(server.workers.wait(timeout: .now() + 2), .success)
    }

    func testDirectoryBrowserDefaultPathUsesTheWorkerHome() async throws {
        let server = try Server()
        let initial = try await server.rpc.browse("/")
        try FileManager.default.createDirectory(at: server.root.appendingPathComponent(String(initial.home.dropFirst())), withIntermediateDirectories: true)
        let listing = try await server.rpc.browse()
        XCTAssertEqual(listing.path, initial.home)
        XCTAssertEqual(listing.home, initial.home)
        XCTAssertTrue(listing.entries.isEmpty)
        XCTAssertEqual(server.connections, 2)
    }


    func testLostStagingAcknowledgementStillAllowsOwnedCleanup() async throws {
        let server = try Server()
        let rpc = FileRPC {
            let connection = try server.connect()
            if server.connections == 1 { shutdown(connection.fileDescriptor, SHUT_RD) }
            return connection
        }
        do {
            _ = try await rpc.createUploadStaging("/.nativepipe-upload-12345678-1234-1234-1234-123456789abc")
            XCTFail("Lost response cannot acknowledge creation")
        } catch { }
        XCTAssertEqual(server.workers.wait(timeout: .now() + 2), .success)
        XCTAssertEqual(server.connections, 2)
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: server.root.path).isEmpty)
    }

    @MainActor
    func testExactTargetUploadPublishesFilesFoldersAndEmptyDirectories() async throws {
        let server = try Server(), files = FileRPCUserAccess(rpc: server.rpc)
        let local = FileManager.default.temporaryDirectory.appendingPathComponent("exact-upload-\(UUID())")
        try FileManager.default.createDirectory(at: local, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: local) }
        try FileManager.default.createDirectory(at: local.appendingPathComponent("empty"), withIntermediateDirectories: false)
        let data = Data(repeating: 0x42, count: 1024 * 1024 + 3)
        try data.write(to: local.appendingPathComponent("hello # 世界"))
        try Data().write(to: local.appendingPathComponent(".nativepipe-owner"))
        let recorder = FileProgressRecorder()
        try await files.uploadFile(local, to: RemoteFileURL.make("/different folder"), progress: { recorder.append($0) })
        let folder = server.root.appendingPathComponent("different folder")
        XCTAssertEqual(try Data(contentsOf: folder.appendingPathComponent("hello # 世界")), data)
        var directory: ObjCBool = false
        XCTAssertTrue(FileManager.default.fileExists(atPath: folder.appendingPathComponent("empty").path, isDirectory: &directory))
        XCTAssertTrue(directory.boolValue)
        XCTAssertEqual(recorder.values.last?.isComplete, true)
        XCTAssertEqual(recorder.values.last?.bytesTransferred, UInt64(data.count))
        XCTAssertEqual(recorder.values.map(\.bytesTransferred), recorder.values.map(\.bytesTransferred).sorted())
        try await files.uploadFile(local.appendingPathComponent("hello # 世界"), to: RemoteFileURL.make("/renamed"))
        XCTAssertEqual(try Data(contentsOf: server.root.appendingPathComponent("renamed")), data)
        XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: server.root.path).contains { $0.hasPrefix(".nativepipe-upload-") })
    }

    @MainActor
    func testExactUploadConflictAndUnsupportedTreeLeaveNoPartialDestinationOrStaging() async throws {
        let server = try Server(), files = FileRPCUserAccess(rpc: server.rpc)
        let local = FileManager.default.temporaryDirectory.appendingPathComponent("failed-upload-\(UUID())")
        try FileManager.default.createDirectory(at: local, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: local) }
        try Data("new".utf8).write(to: local.appendingPathComponent("file"))
        let target = server.root.appendingPathComponent("target")
        try Data("old".utf8).write(to: target)
        do { try await files.uploadFile(local, to: RemoteFileURL.make("/target")); XCTFail("Never overwrite an existing file") } catch { }
        XCTAssertEqual(try Data(contentsOf: target), Data("old".utf8))
        try FileManager.default.createSymbolicLink(at: local.appendingPathComponent("unsupported-link"), withDestinationURL: target)
        let progress = FileProgressRecorder()
        do {
            try await files.uploadFile(local, to: RemoteFileURL.make("/absent"), progress: { progress.append($0) })
            XCTFail("Symbolic links cannot be recursively uploaded")
        } catch { }
        XCTAssertFalse(FileManager.default.fileExists(atPath: server.root.appendingPathComponent("absent").path))
        XCTAssertFalse(progress.values.contains(where: \.isComplete))
        XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: server.root.path).contains { $0.hasPrefix(".nativepipe-upload-") })
        XCTAssertEqual(server.workers.wait(timeout: .now() + 2), .success)
    }

    @MainActor
    func testExactUploadCancellationCleansOwnedPartialTree() async throws {
        let server = try Server(), files = FileRPCUserAccess(rpc: server.rpc)
        let local = FileManager.default.temporaryDirectory.appendingPathComponent("cancel-upload-\(UUID())")
        try Data(repeating: 0x33, count: 8 * 1024 * 1024).write(to: local)
        defer { try? FileManager.default.removeItem(at: local) }
        let cancellation = UploadTaskCancellation(), progress = FileProgressRecorder()
        let task = Task {
            try await files.uploadFile(local, to: RemoteFileURL.make("/cancelled")) { sample in
                progress.append(sample)
                if sample.bytesTransferred > 0 { cancellation.cancel() }
            }
        }
        cancellation.set(task)
        do { try await task.value; XCTFail("Cancelled upload must fail") } catch is CancellationError { }
        XCTAssertEqual(server.workers.wait(timeout: .now() + 2), .success)
        XCTAssertFalse(FileManager.default.fileExists(atPath: server.root.appendingPathComponent("cancelled").path))
        XCTAssertFalse(progress.values.contains(where: \.isComplete))
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: server.root.path).isEmpty)
    }

    @MainActor
    func testExactUploadRejectsReadOnlyAndSymlinkedRemoteParents() async throws {
        let local = FileManager.default.temporaryDirectory.appendingPathComponent("readonly-upload-\(UUID())")
        try Data("bytes".utf8).write(to: local)
        defer { try? FileManager.default.removeItem(at: local) }
        let readOnly = try Server(readOnly: true)
        let listing = try await readOnly.rpc.browse("/")
        XCTAssertTrue(listing.entries.isEmpty)
        do { try await FileRPCUserAccess(rpc: readOnly.rpc).uploadFile(local, to: RemoteFileURL.make("/denied")); XCTFail("Read-only writes fail") }
        catch FileRPC.Failure.remote(let code) { XCTAssertEqual(code, 30) }
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: readOnly.root.path).isEmpty)
        let server = try Server()
        try FileManager.default.createSymbolicLink(at: server.root.appendingPathComponent("link"), withDestinationURL: readOnly.root)
        do { try await FileRPCUserAccess(rpc: server.rpc).uploadFile(local, to: RemoteFileURL.make("/link/escape")); XCTFail("No parent link following") } catch { }
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: readOnly.root.path).isEmpty)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: server.root.path), ["link"])
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
