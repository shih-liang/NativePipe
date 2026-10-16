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
        let url = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)
        XCTAssertEqual(url.path, mount + "/" + token + "/a.txt")
        let folder = try XCTUnwrap(server.share(directory))
        XCTAssertTrue(folder.hasDirectoryPath)
        XCTAssertEqual(folder.lastPathComponent, directory.lastPathComponent)
        let fifo = directory.appendingPathComponent("pipe").path
        XCTAssertEqual(mkfifo(fifo, 0o600), 0)
        XCTAssertNil(server.share(URL(fileURLWithPath: fifo)), "only files and folders")
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

        try await expect(guest, Opcode.open, FileSession.encode(0) + path(token, ""), Status.eisdir)
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
        try await expect(guest, Opcode.open, FileSession.encode(0) + path(token, "doc.txt"), Status.enoent)
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

    // MARK: Writing

    private enum Linux { static let wrOnly: UInt32 = 1, rdWr: UInt32 = 2, create: UInt32 = 0o100, exclusive: UInt32 = 0o200, truncate: UInt32 = 0o1000 }

    private func writeBody(_ handle: UInt64, _ offset: UInt64) -> Data { encode64(handle) + encode64(offset) }
    private func write(_ guest: FileSession, _ handle: UInt64, offset: UInt64, _ bytes: Data) async throws -> UInt32 {
        let stream = try await guest.request(Opcode.write, body: writeBody(handle, offset))
        try await stream.send(bytes, end: true)
        guard let response = try await stream.receive(), case .response(let status) = response.kind else { throw POSIXError(.EPROTO) }
        XCTAssertEqual(status, 0)
        return FileSession.decode32(response.body, at: 0)
    }
    private func create(_ guest: FileSession, _ token: String, _ name: String, flags: UInt32 = 0o101,
                        mode: UInt32 = 0o644, status: UInt32 = 0) async throws -> UInt64? {
        let (body, _) = try await call(guest, Opcode.create, FileSession.encode(flags) + FileSession.encode(mode) + path(token, name), status: status)
        return status == 0 ? body.withUnsafeBytes { UInt64(littleEndian: $0.loadUnaligned(as: UInt64.self)) } : nil
    }
    private func renameBody(_ token: String, _ old: String, _ new: String, flags: UInt32 = 0) -> Data {
        FileSession.encode(flags) + Data([UInt8(old.utf8.count & 0xff), UInt8(old.utf8.count >> 8)]) +
            Data(token.utf8) + Data(old.utf8) + Data(new.utf8)
    }
    private func setattrBody(_ valid: UInt32, mode: UInt32 = 0, size: UInt64 = 0, mtime: Int64 = 0,
                             handle: UInt64 = 0, _ token: String, _ name: String) -> Data {
        FileSession.encode(valid) + FileSession.encode(mode) + encode64(size) + encode64(0) + FileSession.encode(0) +
            encode64(UInt64(bitPattern: mtime)) + FileSession.encode(0) + encode64(handle) + path(token, name)
    }
    private func names(_ guest: FileSession, _ token: String) async throws -> [String] {
        try await entries(guest, Data(token.utf8)).map(\.0)
    }
    private func contents(_ url: URL) -> String { (try? String(contentsOf: url, encoding: .utf8)) ?? "<missing>" }

    func testWritesTruncationAndAttributes() async throws {
        let server = DropFileServer(stagingDirectory: directory.appendingPathComponent("staging"))
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        let file = try write("doc.txt", "hello world")
        _ = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)

        let handle = try await open(guest, token, "doc.txt", flags: Linux.rdWr)
        let written = try await write(guest, handle, offset: 6, Data("drops".utf8))
        XCTAssertEqual(written, 5)
        XCTAssertEqual(contents(file), "hello drops", "writes land in the Mac file")
        let readBack = try await read(guest, handle, offset: 0, length: 64)
        XCTAssertEqual(readBack, Data("hello drops".utf8))
        // A read-only handle refuses writes, as the Mac file system would.
        let readOnly = try await open(guest, token, "doc.txt")
        let refused = try await guest.request(Opcode.write, body: writeBody(readOnly, 0))
        try await refused.send(Data("x".utf8), end: true)
        let refusal = try await refused.receive()
        XCTAssertEqual(refusal?.kind, .response(status: Status.ebadf))

        let truncated = try await attributes(guest, Opcode.setattr, setattrBody(DropFileServer.Set.size, size: 5, token, "doc.txt"))
        XCTAssertEqual(truncated.size, 5)
        XCTAssertEqual(contents(file), "hello")
        let touched = try await attributes(guest, Opcode.setattr,
                                           setattrBody(DropFileServer.Set.mtime | DropFileServer.Set.mode, mode: 0o600, mtime: 1_000_000, token, "doc.txt"))
        XCTAssertEqual(touched.modified.tv_sec, 1_000_000)
        XCTAssertEqual(touched.mode, 0o600)
        try await expect(guest, Opcode.setattr, setattrBody(DropFileServer.Set.uid, token, "doc.txt"), Status.eperm)
        // By handle: fstat of the open file.
        let byHandle = try await attributes(guest, Opcode.setattr, setattrBody(DropFileServer.Set.handle, handle: handle, token, "doc.txt"))
        XCTAssertEqual(byHandle.size, 5)
        try await expect(guest, Opcode.fsync, encode64(handle), 0)
        let truncating = try await open(guest, token, "doc.txt", flags: Linux.wrOnly | Linux.truncate)
        XCTAssertEqual(contents(file), "")
        try await expect(guest, Opcode.release, encode64(truncating), 0)
    }

    /// gedit, LibreOffice and friends: write a temporary file, rename it over.
    func testSavingThroughATemporaryFileReplacesTheMacFile() async throws {
        let staging = directory.appendingPathComponent("staging")
        let server = DropFileServer(stagingDirectory: staging)
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        let file = try write("doc.txt", "old")
        _ = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)

        let created = try await create(guest, token, ".goutputstream-1")
        let temporary = try XCTUnwrap(created)
        _ = try await write(guest, temporary, offset: 0, Data("new contents".utf8))
        let listed = try await names(guest, token)
        XCTAssertEqual(Set(listed), ["doc.txt", ".goutputstream-1"])
        XCTAssertEqual(contents(file), "old", "nothing reaches the Mac file before the rename")
        XCTAssertFalse(FileManager.default.fileExists(atPath: directory.appendingPathComponent(".goutputstream-1").path),
                       "temporary files stay out of the Mac folder")
        try await expect(guest, Opcode.rename, renameBody(token, ".goutputstream-1", "doc.txt", flags: DropFileServer.RenameFlag.noReplace), Status.eexist)
        try await expect(guest, Opcode.rename, renameBody(token, ".goutputstream-1", "doc.txt"), 0)
        XCTAssertEqual(contents(file), "new contents")
        let after = try await names(guest, token)
        XCTAssertEqual(after, ["doc.txt"])
        // The renamed handle now writes the Mac file's new contents.
        _ = try await write(guest, temporary, offset: 0, Data("N".utf8))
        XCTAssertEqual(contents(file), "New contents")

        try await expect(guest, Opcode.rename, renameBody(token, "missing", "doc.txt"), Status.enoent)
        try await expect(guest, Opcode.rename, renameBody(token, "doc.txt", "x", flags: DropFileServer.RenameFlag.exchange), Status.einval)
        server.remove(token: token)
        XCTAssertFalse(FileManager.default.fileExists(atPath: staging.appendingPathComponent(token).path))
    }

    /// vim with backupcopy=no: rename the file aside, then write a new one.
    func testBackupRenameKeepsTheMacFileInPlace() async throws {
        let server = DropFileServer(stagingDirectory: directory.appendingPathComponent("staging"))
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        let file = try write("doc.txt", "version 1")
        _ = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)

        try await expect(guest, Opcode.rename, renameBody(token, "doc.txt", "doc.txt~"), 0)
        XCTAssertEqual(contents(file), "version 1", "the Mac file is not moved")
        let renamed = try await names(guest, token)
        XCTAssertEqual(renamed, ["doc.txt~"])
        try await expect(guest, Opcode.lookup, path(token, "doc.txt"), Status.enoent)

        let recreated = try await create(guest, token, "doc.txt", flags: Linux.wrOnly | Linux.create | Linux.exclusive)
        let fresh = try XCTUnwrap(recreated)
        _ = try await write(guest, fresh, offset: 0, Data("v2".utf8))
        XCTAssertEqual(contents(file), "v2")
        let backup = try await open(guest, token, "doc.txt~")
        let old = try await read(guest, backup, offset: 0, length: 64)
        XCTAssertEqual(old, Data("version 1".utf8))
        let both = try await names(guest, token)
        XCTAssertEqual(Set(both), ["doc.txt", "doc.txt~"])
        _ = try await create(guest, token, "doc.txt", flags: Linux.wrOnly | Linux.create | Linux.exclusive, status: Status.eexist)
    }

    func testUnlinkHidesButKeepsTheMacFile() async throws {
        let server = DropFileServer(stagingDirectory: directory.appendingPathComponent("staging"))
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        let file = try write("doc.txt", "keep me")
        _ = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)
        _ = try await create(guest, token, "scratch")
        try await expect(guest, Opcode.unlink, path(token, "scratch"), 0)
        try await expect(guest, Opcode.unlink, path(token, "doc.txt"), 0)
        XCTAssertEqual(contents(file), "keep me")
        let none = try await names(guest, token)
        XCTAssertEqual(none, [])
        try await expect(guest, Opcode.unlink, path(token, "doc.txt"), Status.enoent)
    }

    // MARK: Folders

    func testFoldersAreServedInPlaceWithoutFollowingLinksOut() async throws {
        let server = DropFileServer(stagingDirectory: directory.appendingPathComponent("staging"))
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        let folder = directory.appendingPathComponent("Project")
        try FileManager.default.createDirectory(at: folder.appendingPathComponent("src"), withIntermediateDirectories: true)
        try Data("int main;".utf8).write(to: folder.appendingPathComponent("src/main.c"))
        let outside = directory.appendingPathComponent("Secret")
        try FileManager.default.createDirectory(at: outside, withIntermediateDirectories: true)
        try Data("private".utf8).write(to: outside.appendingPathComponent("key"))
        XCTAssertEqual(symlink(outside.path, folder.appendingPathComponent("escape").path), 0)

        let url = try XCTUnwrap(server.share(folder))
        XCTAssertTrue(url.hasDirectoryPath)
        let token = try XCTUnwrap(server.tokens.first)
        let top = try await entries(guest, Data(token.utf8))
        XCTAssertEqual(top.map(\.0), ["Project"])
        XCTAssertEqual(top.first?.1.kind, .directory)
        let inside = try await entries(guest, path(token, "Project"))
        XCTAssertEqual(Set(inside.map(\.0)), ["src", "escape"])
        XCTAssertEqual(inside.first { $0.0 == "escape" }?.1.kind, .link)

        // Reading and writing go to the Mac folder itself.
        let main = try await open(guest, token, "Project/src/main.c", flags: Linux.rdWr)
        _ = try await write(guest, main, offset: 4, Data("MAIN".utf8))
        XCTAssertEqual(contents(folder.appendingPathComponent("src/main.c")), "int MAIN;")
        let created = try await create(guest, token, "Project/src/util.c")
        let createdHandle = try XCTUnwrap(created)
        _ = try await write(guest, createdHandle, offset: 0, Data("x".utf8))
        XCTAssertEqual(contents(folder.appendingPathComponent("src/util.c")), "x")

        // A link is shown as a link, never followed by the host.
        let link = try await call(guest, Opcode.readlink, path(token, "Project/escape")).1
        XCTAssertEqual(String(decoding: link, as: UTF8.self), outside.path)
        try await expect(guest, Opcode.lookup, path(token, "Project/escape/key"), 40) // ELOOP: no way out
        try await expect(guest, Opcode.open, FileSession.encode(0) + path(token, "Project/escape/key"), 40)
        try await expect(guest, Opcode.open, FileSession.encode(0) + path(token, "Project/escape"), 40)
        try await expect(guest, Opcode.lookup, path(token, "Project/../Secret"), Status.enoent)

        // Directories and links are created and removed in place.
        let made = try await attributes(guest, Opcode.mkdir, FileSession.encode(0o755) + path(token, "Project/build"))
        XCTAssertEqual(made.kind, .directory)
        let target = Data("../src/main.c".utf8)
        let linked = try await attributes(guest, Opcode.symlink,
                                          Data([UInt8(target.count), 0]) + target + path(token, "Project/build/main.c"))
        XCTAssertEqual(linked.kind, .link)
        XCTAssertEqual(try FileManager.default.destinationOfSymbolicLink(atPath: folder.appendingPathComponent("build/main.c").path),
                       "../src/main.c")
        try await expect(guest, Opcode.rename, renameBody(token, "Project/src/util.c", "Project/build/util.c"), 0)
        XCTAssertTrue(FileManager.default.fileExists(atPath: folder.appendingPathComponent("build/util.c").path))
        try await expect(guest, Opcode.rename, renameBody(token, "Project/build/util.c", "Project/src/main.c",
                                                          flags: DropFileServer.RenameFlag.noReplace), Status.eexist)
        try await expect(guest, Opcode.rmdir, path(token, "Project/build"), 39) // ENOTEMPTY
        try await expect(guest, Opcode.unlink, path(token, "Project/build/main.c"), 0)
        try await expect(guest, Opcode.unlink, path(token, "Project/build/util.c"), 0)
        try await expect(guest, Opcode.rmdir, path(token, "Project/build"), 0)
        XCTAssertFalse(FileManager.default.fileExists(atPath: folder.appendingPathComponent("build").path))

        // The dropped folder itself stays put.
        try await expect(guest, Opcode.rmdir, path(token, "Project"), Status.ebusy)
        try await expect(guest, Opcode.rename, renameBody(token, "Project", "Other"), Status.eperm)
        try await expect(guest, Opcode.rename, renameBody(token, "Project/src", "Project"), Status.ebusy)
        _ = try await create(guest, token, "Sibling", status: Status.eperm)
        server.remove(token: token)
        XCTAssertTrue(FileManager.default.fileExists(atPath: folder.appendingPathComponent("src/main.c").path),
                      "forgetting a folder drop never touches the folder")
    }

    // MARK: Reclamation

    func testIdleDropsAreReclaimedButOpenOnesNever() async throws {
        let staging = directory.appendingPathComponent("staging")
        let server = DropFileServer(stagingDirectory: staging)
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        _ = try XCTUnwrap(server.share(try write("a.txt", "a")))
        let token = try XCTUnwrap(server.tokens.first)
        let handle = try await open(guest, token, "a.txt")
        XCTAssertEqual(server.inventory().first?.openFiles, 1)

        let later = Date().addingTimeInterval(3600)
        XCTAssertEqual(server.reclaim(idleLimit: 60, stagedLimit: 0, now: later), [], "an open drop stays")
        XCTAssertEqual(server.removeIdle(), [])
        try await expect(guest, Opcode.release, encode64(handle), 0)
        XCTAssertEqual(server.inventory().first?.openFiles, 0)
        XCTAssertEqual(server.reclaim(idleLimit: 60, stagedLimit: .max), [], "not idle long enough yet")
        XCTAssertEqual(server.reclaim(idleLimit: 60, stagedLimit: .max, now: later), [token])
        XCTAssertEqual(server.tokens, [])
        try await expect(guest, Opcode.lookup, path(token, ""), Status.enoent)
    }

    func testStagedSpaceLimitReclaimsTheLeastRecentlyUsedIdleDrops() async throws {
        let staging = directory.appendingPathComponent("staging")
        let server = DropFileServer(stagingDirectory: staging)
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        var tokens: [String] = []
        for name in ["old.txt", "new.txt"] {
            _ = try XCTUnwrap(server.share(try write(name, name)))
            let token = try XCTUnwrap(Set(server.tokens).subtracting(tokens).first)
            tokens.append(token)
            let created = try await create(guest, token, "." + name + ".swp")
            let temporary = try XCTUnwrap(created)
            _ = try await write(guest, temporary, offset: 0, Data(count: 200_000))
            try await expect(guest, Opcode.release, encode64(temporary), 0)
            try await Task.sleep(nanoseconds: 20_000_000)
        }
        let items = server.inventory()
        XCTAssertEqual(items.map(\.token), tokens.reversed(), "most recently used first")
        let perDrop = try XCTUnwrap(items.first?.stagedBytes)
        XCTAssertGreaterThanOrEqual(perDrop, 200_000)
        XCTAssertEqual(server.reclaim(idleLimit: 3600, stagedLimit: perDrop + 1), [tokens[0]])
        XCTAssertEqual(server.tokens, [tokens[1]])
        XCTAssertFalse(FileManager.default.fileExists(atPath: staging.appendingPathComponent(tokens[0]).path))
        XCTAssertEqual(server.removeIdle(), [tokens[1]])
    }

    // MARK: Locks

    private enum LockType { static let read: UInt32 = 0, write: UInt32 = 1, unlock: UInt32 = 2 }
    private func lockBody(_ handle: UInt64, owner: UInt64, _ start: UInt64, _ end: UInt64, _ type: UInt32, flags: UInt32 = 0) -> Data {
        encode64(handle) + encode64(owner) + encode64(start) + encode64(end) + FileSession.encode(type) + FileSession.encode(flags)
    }
    private func macLock(_ url: URL, _ type: Int32, _ start: off_t = 0, _ length: off_t = 0) -> (Int32, Int32) {
        let descriptor = Darwin.open(url.path, O_RDWR)
        var lock = Darwin.flock(l_start: start, l_len: length, l_pid: 0, l_type: Int16(type), l_whence: Int16(SEEK_SET))
        let result = fcntl(descriptor, F_OFD_SETLK, &lock)
        return (descriptor, result == 0 ? 0 : errno)
    }

    func testPosixLocksExcludeMacProgramsAndOtherOwners() async throws {
        let server = DropFileServer()
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        let file = try write("db", String(repeating: "x", count: 100))
        _ = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)
        let first = try await open(guest, token, "db", flags: Linux.rdWr)
        let second = try await open(guest, token, "db", flags: Linux.rdWr)

        try await expect(guest, Opcode.setlk, lockBody(first, owner: 1, 0, 9, LockType.write), 0)
        // Same owner through another open file: POSIX ownership, no conflict.
        try await expect(guest, Opcode.setlk, lockBody(second, owner: 1, 5, 14, LockType.write), 0)
        try await expect(guest, Opcode.setlk, lockBody(second, owner: 2, 0, 0, LockType.read), Status.eagain)
        let conflict = try await call(guest, Opcode.getlk, lockBody(second, owner: 2, 0, 0, LockType.read)).0
        XCTAssertEqual(FileSession.decode32(conflict, at: 16), LockType.write)
        XCTAssertEqual(DropFileServer.u64(conflict, 0), 0)
        XCTAssertEqual(DropFileServer.u64(conflict, 8), 14)

        let (mac, macResult) = macLock(file, F_WRLCK, 3, 1)
        XCTAssertEqual(macResult, EAGAIN, "a Mac program cannot lock what Linux locked")
        Darwin.close(mac)
        let (reader, readerResult) = macLock(file, F_RDLCK, 50, 10)
        XCTAssertEqual(readerResult, 0, "unlocked ranges stay available")
        try await expect(guest, Opcode.setlk, lockBody(first, owner: 3, 55, 56, LockType.write), Status.eagain)
        Darwin.close(reader)

        // close(2) of either open file releases the owner's locks.
        try await expect(guest, Opcode.flush, encode64(second) + encode64(1), 0)
        try await expect(guest, Opcode.setlk, lockBody(second, owner: 2, 0, 0, LockType.read), 0)
        let free = try await call(guest, Opcode.getlk, lockBody(first, owner: 3, 20, 30, LockType.write)).0
        XCTAssertEqual(FileSession.decode32(free, at: 16), LockType.unlock)
        try await expect(guest, Opcode.setlk, lockBody(second, owner: 2, 0, UInt64(Int64.max), LockType.unlock), 0)
        // The session ending releases everything it held.
        try await expect(guest, Opcode.setlk, lockBody(first, owner: 4, 0, UInt64(Int64.max), LockType.write), 0)
        server.detach()
        let (after, afterResult) = macLock(file, F_WRLCK)
        XCTAssertEqual(afterResult, 0)
        Darwin.close(after)
    }

    func testFlockExcludesMacPrograms() async throws {
        let server = DropFileServer()
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        let file = try write("lockfile", "")
        _ = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)
        let handle = try await open(guest, token, "lockfile")
        try await expect(guest, Opcode.setlk, lockBody(handle, owner: 1, 0, UInt64(Int64.max), LockType.write,
                                                       flags: DropFileServer.LockFlag.flock), 0)
        let mac = Darwin.open(file.path, O_RDONLY)
        defer { Darwin.close(mac) }
        XCTAssertEqual(np_drop_flock(mac, LOCK_EX | LOCK_NB), -1)
        try await expect(guest, Opcode.release, encode64(handle), 0)
        XCTAssertEqual(np_drop_flock(mac, LOCK_EX | LOCK_NB), 0, "closing the open file ends its flock")
    }

    func testBlockingLockWaitsForTheMacProgramAndCanBeCancelled() async throws {
        let server = DropFileServer()
        let guest = try await connect(server)
        defer { guest.close(); server.detach() }
        let file = try write("db", "data")
        _ = try XCTUnwrap(server.share(file))
        let token = try XCTUnwrap(server.tokens.first)
        let handle = try await open(guest, token, "db", flags: Linux.rdWr)
        let (mac, macResult) = macLock(file, F_WRLCK)
        XCTAssertEqual(macResult, 0)

        let waiting = try await guest.request(Opcode.setlk, body: lockBody(handle, owner: 1, 0, 3, LockType.write,
                                                                          flags: DropFileServer.LockFlag.wait), end: true)
        let granted = Task { try await waiting.receive() }
        try await Task.sleep(nanoseconds: 100_000_000)
        Darwin.close(mac) // the Mac program unlocks
        let response = try await granted.value
        XCTAssertEqual(response?.kind, .response(status: 0))

        let (other, otherResult) = macLock(file, F_WRLCK, 10, 1)
        XCTAssertEqual(otherResult, 0)
        defer { Darwin.close(other) }
        let cancelled = try await guest.request(Opcode.setlk, body: lockBody(handle, owner: 2, 10, 10, LockType.write,
                                                                            flags: DropFileServer.LockFlag.wait), end: true)
        try await Task.sleep(nanoseconds: 50_000_000)
        cancelled.cancel()
        // The host stops waiting; the session stays healthy.
        let still = try await attributes(guest, Opcode.lookup, path(token, "db"))
        XCTAssertEqual(still.size, 4)
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
            // Save as a Linux editor does: temporary file, write, rename over.
            let (temporaryNode, temporaryHandle) = try driver.create(parent: tokenNode, ".big.bin.swp")
            guard try driver.write(temporaryNode, temporaryHandle, offset: 0, Data("saved from Linux".utf8)) == 16 else {
                throw POSIXError(.EIO)
            }
            try driver.release(temporaryNode, temporaryHandle)
            try driver.rename(parent: tokenNode, ".big.bin.swp", "big.bin")
            return data
        }
        XCTAssertEqual(result, contents)
        XCTAssertEqual(try String(contentsOf: file, encoding: .utf8), "saved from Linux")

        // A dropped folder, walked down through the handler.
        let tree = directory.appendingPathComponent("Tree")
        try FileManager.default.createDirectory(at: tree.appendingPathComponent("a"), withIntermediateDirectories: true)
        try Data("deep".utf8).write(to: tree.appendingPathComponent("a/b.txt"))
        let folderURL = try XCTUnwrap(server.share(tree))
        let folderToken = folderURL.pathComponents[folderURL.pathComponents.count - 2]
        let deep: Data = try await Self.onThread {
            let driver = FuseDriver(fs)
            let tokenNode = try driver.lookup(parent: 1, folderToken)
            let treeNode = try driver.lookup(parent: tokenNode, "Tree")
            guard try driver.readdir(treeNode) == [".", "..", "a"] else { throw POSIXError(.EPROTO) }
            let aNode = try driver.lookup(parent: treeNode, "a")
            let fileNode = try driver.lookup(parent: aNode, "b.txt")
            let handle = try driver.open(fileNode)
            defer { try? driver.release(fileNode, handle) }
            return try driver.read(fileNode, handle, offset: 0, size: 4096)
        }
        XCTAssertEqual(deep, Data("deep".utf8))
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
    private func open(_ guest: FileSession, _ token: String, _ name: String, flags: UInt32 = 0) async throws -> UInt64 {
        let body = try await call(guest, Opcode.open, FileSession.encode(flags) + path(token, name)).0
        XCTAssertEqual(body.count, 8 + DropFileServer.attributeSize)
        return body.withUnsafeBytes { UInt64(littleEndian: $0.loadUnaligned(as: UInt64.self)) }
    }
    private func read(_ guest: FileSession, _ handle: UInt64, offset: UInt64, length: UInt32) async throws -> Data {
        try await call(guest, Opcode.read, readBody(handle, offset, length)).1
    }
    private func entries(_ guest: FileSession, _ body: Data) async throws -> [(String, DropFileServer.Attributes)] {
        var data = try await call(guest, Opcode.list, FileSession.encode(0) + body).1
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
    func create(parent: UInt64, _ name: String) throws -> (UInt64, UInt64) {
        var body = Data()
        for value: UInt32 in [0o101, 0o644, 0o022, 0] { withUnsafeBytes(of: value.littleEndian) { body.append(contentsOf: $0) } }
        body.append(Data(name.utf8) + Data([0]))
        let reply = try call(35, node: parent, body)
        return (u64(reply, 0), u64(reply, 128))
    }
    func write(_ node: UInt64, _ handle: UInt64, offset: UInt64, _ bytes: Data) throws -> UInt32 {
        var body = Data()
        for value in [handle, offset] { withUnsafeBytes(of: value.littleEndian) { body.append(contentsOf: $0) } }
        withUnsafeBytes(of: UInt32(bytes.count).littleEndian) { body.append(contentsOf: $0) }
        body.append(Data(count: 20))
        body.append(bytes)
        let reply = try call(16, node: node, body)
        return reply.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(as: UInt32.self)) }
    }
    func rename(parent: UInt64, _ old: String, _ new: String) throws {
        var body = Data()
        withUnsafeBytes(of: parent.littleEndian) { body.append(contentsOf: $0) }
        body.append(Data(old.utf8) + Data([0]) + Data(new.utf8) + Data([0]))
        _ = try call(12, node: parent, body)
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
