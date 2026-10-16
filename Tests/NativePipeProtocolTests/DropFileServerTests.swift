import CNativePipeFileRPC
import Darwin
import Foundation
import XCTest
@testable import NativePipeProtocol

final class DropFileServerTests: XCTestCase {
    private typealias Opcode = DropFileServer.Opcode
    private typealias Status = DropFileServer.Status
    private let mount = "/run/nativepipe/drops/1000"
    private var directory: URL!

    override func setUpWithError() throws {
        directory = FileManager.default.temporaryDirectory.appendingPathComponent("drop-server-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }
    override func tearDownWithError() throws { try? FileManager.default.removeItem(at: directory) }

    func testWireConstantsMatchC() {
        XCTAssertEqual(DropFileServer.tokenLength, Int(NP_DROP_TOKEN))
        XCTAssertEqual(DropFileServer.attributeSize, Int(NP_DROP_ATTR))
        XCTAssertEqual(DropFileServer.readMaximum, Int(NP_DROP_READ_MAX))
        XCTAssertEqual(DropFileServer.nameMaximum, Int(NP_DROP_NAME_MAX))
        XCTAssertEqual(Opcode.release, UInt16(NP_DROP_RELEASE.rawValue))
        XCTAssertEqual(DropFileServer.Notification.mounted, UInt16(NP_DROP_MOUNTED.rawValue))
        XCTAssertEqual(Status.from(ESTALE), 116)
        XCTAssertEqual(Status.from(ELOOP), 40)
        let attributes = DropFileServer.Attributes(kind: .file, mode: 0o644, size: 1 << 40,
                                                   modified: timespec(tv_sec: -5, tv_nsec: 7), fileID: 99)
        var c = np_drop_attr()
        let encoded = attributes.encoded()
        XCTAssertEqual(encoded.withUnsafeBytes { np_drop_attr_decode($0.bindMemory(to: UInt8.self).baseAddress, 40, &c) }, 0)
        XCTAssertEqual(c.size, 1 << 40); XCTAssertEqual(c.mtime_seconds, -5); XCTAssertEqual(c.file_id, 99)
        XCTAssertEqual(DropFileServer.Attributes.decode(encoded), attributes)
    }

    func testSharingNeedsAMountAndARegularFile() async throws {
        let server = DropFileServer()
        let file = try write("a.txt", "abc")
        XCTAssertNil(server.share(file), "no guest mount yet")
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        XCTAssertNil(server.share(directory), "directories stay on virtiofs")
        let url = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)
        XCTAssertEqual(url.path, mount + "/" + token + "/a.txt")
    }

    func testLookupListOpenReadRelease() async throws {
        let server = DropFileServer()
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        let file = try write("notes # 1.txt", "hello drops")
        chmod(file.path, 0o640)
        _ = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)

        let directoryAttributes = try await attributes(guest, Opcode.lookup, path(token, ""))
        XCTAssertEqual(directoryAttributes.kind, .directory)
        let fileAttributes = try await attributes(guest, Opcode.lookup, path(token, "notes # 1.txt"))
        XCTAssertEqual(fileAttributes.kind, .file)
        XCTAssertEqual(fileAttributes.size, 11)
        XCTAssertEqual(fileAttributes.mode, 0o640)
        try await expect(guest, Opcode.lookup, path(token, "other"), Status.enoent)
        try await expect(guest, Opcode.lookup, path(UUID().uuidString, ""), Status.enoent)
        try await expect(guest, Opcode.lookup, path(token, ".."), Status.enoent)

        let root = try await entries(guest, Data())
        XCTAssertEqual(root.map(\.0), [token])
        let listed = try await entries(guest, Data(token.utf8))
        XCTAssertEqual(listed.map(\.0), ["notes # 1.txt"])
        XCTAssertEqual(listed.first?.1.size, 11)

        try await expect(guest, Opcode.open, path(token, ""), Status.eisdir)
        let handle = try await open(guest, token, "notes # 1.txt")
        let middle = try await read(guest, handle, offset: 6, length: 100)
        XCTAssertEqual(middle, Data("drops".utf8))
        let end = try await read(guest, handle, offset: 11, length: 10)
        XCTAssertEqual(end, Data())
        try await expect(guest, Opcode.read, readBody(handle, 0, UInt32(DropFileServer.readMaximum + 1)), Status.einval)
        try await expect(guest, Opcode.release, encode64(handle), 0)
        try await expect(guest, Opcode.read, readBody(handle, 0, 4), Status.ebadf)
        try await expect(guest, Opcode.release, encode64(handle), Status.ebadf)

        server.remove(token: token)
        try await expect(guest, Opcode.lookup, path(token, ""), Status.enoent)
    }

    /// Path semantics: like a second Mac app, an open handle keeps the file it
    /// opened while lookups and new opens see the replacement.
    func testAtomicReplacementIsSeenByPathNotByOpenHandles() async throws {
        let server = DropFileServer()
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        let file = try write("doc.txt", "old")
        _ = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)
        let old = try await open(guest, token, "doc.txt")

        let replacement = directory.appendingPathComponent(".doc.txt.tmp")
        try Data("new contents".utf8).write(to: replacement)
        XCTAssertEqual(rename(replacement.path, file.path), 0)

        let oldData = try await read(guest, old, offset: 0, length: 64)
        XCTAssertEqual(oldData, Data("old".utf8))
        let current = try await attributes(guest, Opcode.lookup, path(token, "doc.txt"))
        XCTAssertEqual(current.size, 12)
        let fresh = try await open(guest, token, "doc.txt")
        let freshData = try await read(guest, fresh, offset: 0, length: 64)
        XCTAssertEqual(freshData, Data("new contents".utf8))

        // Deleted outright: gone by path, still readable through the old handle.
        XCTAssertEqual(unlink(file.path), 0)
        try await expect(guest, Opcode.lookup, path(token, "doc.txt"), Status.enoent)
        try await expect(guest, Opcode.open, path(token, "doc.txt"), Status.enoent)
        let stillOld = try await read(guest, old, offset: 0, length: 64)
        XCTAssertEqual(stillOld, Data("old".utf8))
    }

    func testANewSessionInvalidatesTheOldOnesHandles() async throws {
        let server = DropFileServer()
        let first = try await connect(server)
        let file = try write("f", "data")
        _ = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)
        let handle = try await open(first, token, "f")
        let second = try await connect(server)
        defer { first.close(); second.close(); server.detach() }
        try await expect(second, Opcode.read, readBody(handle, 0, 4), Status.ebadf)
        // Drops outlive sessions: the guest compositor may restart.
        let attributes = try await attributes(second, Opcode.lookup, path(token, "f"))
        XCTAssertEqual(attributes.size, 4)
    }

    // MARK: End to end through the C FUSE handler

    func testCFuseHandlerReadsThroughSwiftServer() async throws {
        let server = DropFileServer()
        var descriptors: [Int32] = [-1, -1]
        XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors), 0)
        server.attach(try SocketConnection(owning: descriptors[0]))
        defer { server.detach() }
        let guest = try XCTUnwrap(np_file2_session_start(descriptors[1], NP_FILE2_ROLE_GUEST, nil))
        XCTAssertEqual(np_file2_notify(guest, UInt16(NP_DROP_MOUNTED.rawValue), mount, mount.utf8.count), 0)
        try await waitUntilReady(server)
        let contents = Data((0..<300_000).map { UInt8(truncatingIfNeeded: $0 &* 7) })
        let file = directory.appendingPathComponent("big.bin")
        try contents.write(to: file)
        let url = try XCTUnwrap(server.share(file))
        let token = url.pathComponents[url.pathComponents.count - 2]

        let fs = try XCTUnwrap(np_drop_fs_create(1000, 1000))
        np_drop_fs_set_session(fs, guest)
        defer {
            np_drop_fs_set_session(fs, nil)
            np_file2_session_close(guest)
            np_drop_fs_destroy(fs)
        }
        let result: Data = try await Self.onThread {
            let driver = FuseDriver(fs)
            let tokenNode = try driver.lookup(parent: 1, token)
            let fileNode = try driver.lookup(parent: tokenNode, "big.bin")
            let names = try driver.readdir(tokenNode)
            guard names == [".", "..", "big.bin"] else { throw POSIXError(.EPROTO) }
            let rootNames = try driver.readdir(1)
            guard rootNames.contains(token) else { throw POSIXError(.EPROTO) }
            let handle = try driver.open(fileNode)
            var data = Data()
            while true {
                let chunk = try driver.read(fileNode, handle, offset: UInt64(data.count), size: 131_072)
                if chunk.isEmpty { break }
                data.append(chunk)
            }
            try driver.release(fileNode, handle)
            return data
        }
        XCTAssertEqual(result, contents)
    }

    // MARK: Helpers

    private func write(_ name: String, _ text: String) throws -> URL {
        let url = directory.appendingPathComponent(name)
        try Data(text.utf8).write(to: url)
        return url
    }

    private func connect(_ server: DropFileServer) async throws -> FileSession {
        var descriptors: [Int32] = [-1, -1]
        guard socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors) == 0 else { throw POSIXError(.EIO) }
        server.attach(try SocketConnection(owning: descriptors[0]))
        let guest = FileSession(connection: try SocketConnection(owning: descriptors[1]), role: .guest)
        guest.start()
        try await guest.notify(DropFileServer.Notification.mounted, body: Data(mount.utf8))
        try await waitUntilReady(server)
        return guest
    }

    private func waitUntilReady(_ server: DropFileServer) async throws {
        for _ in 0..<500 {
            if server.isReady { return }
            try await Task.sleep(nanoseconds: 2_000_000)
        }
        throw POSIXError(.ETIMEDOUT)
    }

    private func path(_ token: String, _ name: String) -> Data { Data(token.utf8) + Data(name.utf8) }
    private func encode64(_ value: UInt64) -> Data { DropFileServer.Attributes.encode64(value) }
    private func readBody(_ handle: UInt64, _ offset: UInt64, _ length: UInt32) -> Data {
        encode64(handle) + encode64(offset) + FileSession.encode(length)
    }

    /// Returns the response body and any DATA, after checking the status.
    private func call(_ guest: FileSession, _ opcode: UInt16, _ body: Data, status: UInt32 = 0) async throws -> (Data, Data) {
        let stream = try await guest.request(opcode, body: body, end: true)
        guard let response = try await stream.receive(), case .response(let actual) = response.kind else {
            throw POSIXError(.EPROTO)
        }
        XCTAssertEqual(actual, status)
        var data = Data()
        while let message = try await stream.receive() { data.append(message.body) }
        return (response.body, data)
    }
    private func expect(_ guest: FileSession, _ opcode: UInt16, _ body: Data, _ status: UInt32) async throws {
        _ = try await call(guest, opcode, body, status: status)
    }
    private func attributes(_ guest: FileSession, _ opcode: UInt16, _ body: Data) async throws -> DropFileServer.Attributes {
        let body = try await call(guest, opcode, body).0
        return try XCTUnwrap(DropFileServer.Attributes.decode(body))
    }
    private func open(_ guest: FileSession, _ token: String, _ name: String) async throws -> UInt64 {
        let body = try await call(guest, Opcode.open, path(token, name)).0
        XCTAssertEqual(body.count, 8 + DropFileServer.attributeSize)
        return body.withUnsafeBytes { UInt64(littleEndian: $0.loadUnaligned(as: UInt64.self)) }
    }
    private func read(_ guest: FileSession, _ handle: UInt64, offset: UInt64, length: UInt32) async throws -> Data {
        try await call(guest, Opcode.read, readBody(handle, offset, length)).1
    }
    private func entries(_ guest: FileSession, _ body: Data) async throws -> [(String, DropFileServer.Attributes)] {
        var data = try await call(guest, Opcode.list, body).1
        var result: [(String, DropFileServer.Attributes)] = []
        while data.count >= 2 {
            let length = Int(data[data.startIndex]) | Int(data[data.startIndex + 1]) << 8
            let name = String(decoding: data.dropFirst(2).prefix(length), as: UTF8.self)
            let attributes = try XCTUnwrap(DropFileServer.Attributes.decode(Data(data.dropFirst(2 + length).prefix(40))))
            result.append((name, attributes))
            data = Data(data.dropFirst(2 + length + 40))
        }
        return result
    }

    /// The C handler blocks on its session; keep it off the cooperative pool.
    private static func onThread<T: Sendable>(_ body: @escaping @Sendable () throws -> T) async throws -> T {
        try await withCheckedThrowingContinuation { continuation in
            Thread.detachNewThread { continuation.resume(with: Result { try body() }) }
        }
    }
}

/// Builds Linux FUSE requests for `np_drop_fs_handle` and decodes replies.
private final class FuseDriver: @unchecked Sendable {
    private let fs: OpaquePointer
    private var unique: UInt64 = 1
    private var reply = Data()

    init(_ fs: OpaquePointer) { self.fs = fs }

    private static let capture: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<iovec>?, Int32) -> Int32 = { context, vector, count in
        let driver = Unmanaged<FuseDriver>.fromOpaque(context!).takeUnretainedValue()
        driver.reply = Data()
        for index in 0..<Int(count) {
            let item = vector![index]
            driver.reply.append(item.iov_base.assumingMemoryBound(to: UInt8.self), count: item.iov_len)
        }
        return 0
    }

    /// Returns the reply body, throwing the reply's errno.
    private func call(_ opcode: UInt32, node: UInt64, _ body: Data) throws -> Data {
        var request = Data()
        func put32(_ value: UInt32) { withUnsafeBytes(of: value.littleEndian) { request.append(contentsOf: $0) } }
        func put64(_ value: UInt64) { withUnsafeBytes(of: value.littleEndian) { request.append(contentsOf: $0) } }
        put32(UInt32(40 + body.count)); put32(opcode); put64(unique); put64(node)
        put32(1000); put32(1000); put32(1); put32(0)
        request.append(body)
        unique += 1
        let context = Unmanaged.passUnretained(self).toOpaque()
        let result = request.withUnsafeBytes { np_drop_fs_handle(fs, $0.baseAddress, $0.count, Self.capture, context) }
        guard result == 0, reply.count >= 16 else { throw POSIXError(.EPROTO) }
        let error = reply.withUnsafeBytes { Int32(littleEndian: $0.loadUnaligned(fromByteOffset: 4, as: Int32.self)) }
        if error != 0 { throw POSIXError(POSIXErrorCode(rawValue: -error) ?? .EIO) }
        return Data(reply.dropFirst(16))
    }
    private func u64(_ data: Data, _ offset: Int) -> UInt64 {
        data.withUnsafeBytes { UInt64(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: UInt64.self)) }
    }

    func lookup(parent: UInt64, _ name: String) throws -> UInt64 {
        u64(try call(1, node: parent, Data(name.utf8) + Data([0])), 0)
    }
    func open(_ node: UInt64) throws -> UInt64 { u64(try call(14, node: node, Data(count: 8)), 0) }
    func read(_ node: UInt64, _ handle: UInt64, offset: UInt64, size: UInt32) throws -> Data {
        var body = Data()
        for value in [handle, offset] { withUnsafeBytes(of: value.littleEndian) { body.append(contentsOf: $0) } }
        withUnsafeBytes(of: size.littleEndian) { body.append(contentsOf: $0) }
        body.append(Data(count: 20))
        return try call(15, node: node, body)
    }
    func release(_ node: UInt64, _ handle: UInt64) throws {
        var body = Data()
        withUnsafeBytes(of: handle.littleEndian) { body.append(contentsOf: $0) }
        body.append(Data(count: 16))
        _ = try call(18, node: node, body)
    }
    func readdir(_ node: UInt64) throws -> [String] {
        var body = Data(count: 40)
        body.replaceSubrange(16..<20, with: withUnsafeBytes(of: UInt32(65536).littleEndian) { Data($0) })
        let data = try call(28, node: node, body)
        var names: [String] = [], position = 0
        while position + 24 <= data.count {
            let length = Int(data.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: position + 16, as: UInt32.self)) })
            names.append(String(decoding: data.dropFirst(position + 24).prefix(length), as: UTF8.self))
            position += (24 + length + 7) & ~7
        }
        return names
    }
}
