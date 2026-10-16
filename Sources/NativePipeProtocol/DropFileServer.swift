import CNativePipeFileRPC
import Foundation
import Darwin

/// Host side of the guest drop filesystem (`common/file_rpc/np_drop_fs.c`).
/// Each dropped file gets a random token; the guest sees it as
/// `<mount>/<token>/<name>`. The host opens the dropped path on demand, as any
/// second Mac app with that file open would: identity is the path, every lookup
/// and open sees the current file, and an open handle keeps the file it opened.
///
/// Writing follows plain POSIX semantics, not any particular save strategy.
/// Other files a guest app creates in a drop's directory (temporary files,
/// backups) live in a private staging directory; renaming one onto the dropped
/// name replaces the Mac file, and unlinking the dropped name only removes it
/// from the guest. Locks the guest takes are taken on the Mac file too: POSIX
/// locks as open-file-description locks per guest lock owner, flock(2) locks on
/// the open file, so Mac and Linux programs that lock exclude each other.
public final class DropFileServer: @unchecked Sendable {
    public enum Opcode {
        public static let lookup: UInt16 = 1, list: UInt16 = 2, open: UInt16 = 3
        public static let read: UInt16 = 4, release: UInt16 = 5, write: UInt16 = 6
        public static let setattr: UInt16 = 7, create: UInt16 = 8, unlink: UInt16 = 9
        public static let rename: UInt16 = 10, fsync: UInt16 = 11, flush: UInt16 = 12
        public static let getlk: UInt16 = 13, setlk: UInt16 = 14
    }
    public enum Notification { public static let mounted: UInt16 = 1 }
    public static let tokenLength = 36
    public static let attributeSize = 40
    public static let readMaximum = 1024 * 1024
    public static let nameMaximum = 255
    public static let setattrSize = 48
    public static let lockSize = 40
    /// FUSE FATTR_* bits carried by SETATTR.
    public enum Set {
        public static let mode: UInt32 = 1, uid: UInt32 = 2, gid: UInt32 = 4, size: UInt32 = 8
        public static let atime: UInt32 = 16, mtime: UInt32 = 32, handle: UInt32 = 64
        public static let atimeNow: UInt32 = 128, mtimeNow: UInt32 = 256
    }
    public enum LockFlag { public static let flock: UInt32 = 1, wait: UInt32 = 2 }
    public enum RenameFlag { public static let noReplace: UInt32 = 1, exchange: UInt32 = 2 }
    /// Linux values on the wire.
    enum LinuxOpen {
        static let accessMode: UInt32 = 0o3, create: UInt32 = 0o100, exclusive: UInt32 = 0o200
        static let truncate: UInt32 = 0o1000, append: UInt32 = 0o2000
    }
    enum LinuxLock { static let read: UInt32 = 0, write: UInt32 = 1, unlock: UInt32 = 2 }
    static let lockToEnd = UInt64(Int64.max)

    /// What a drop exposes: the kind, permission bits, size, modification
    /// time and file identity, in the wire's 40-byte layout.
    public struct Attributes: Equatable, Sendable {
        public enum Kind: UInt8, Sendable { case directory = 1, file = 2 }
        public var kind: Kind
        public var mode: UInt32
        public var size: UInt64
        public var modified: timespec
        public var fileID: UInt64

        public static func == (lhs: Self, rhs: Self) -> Bool {
            lhs.kind == rhs.kind && lhs.mode == rhs.mode && lhs.size == rhs.size &&
                lhs.modified.tv_sec == rhs.modified.tv_sec && lhs.modified.tv_nsec == rhs.modified.tv_nsec &&
                lhs.fileID == rhs.fileID
        }

        public func encoded() -> Data {
            var data = Data(count: DropFileServer.attributeSize)
            data[0] = kind.rawValue
            data.replaceSubrange(4..<8, with: FileSession.encode(mode & 0o7777))
            data.replaceSubrange(8..<16, with: Self.encode64(size))
            data.replaceSubrange(16..<24, with: Self.encode64(UInt64(bitPattern: Int64(modified.tv_sec))))
            data.replaceSubrange(24..<28, with: FileSession.encode(UInt32(modified.tv_nsec)))
            data.replaceSubrange(32..<40, with: Self.encode64(fileID))
            return data
        }

        public static func decode(_ data: Data) -> Attributes? {
            guard data.count >= DropFileServer.attributeSize else { return nil }
            let bytes = Data(data)
            guard let kind = Kind(rawValue: bytes[0]) else { return nil }
            let nanoseconds = FileSession.decode32(bytes, at: 24)
            guard nanoseconds < 1_000_000_000 else { return nil }
            return Attributes(kind: kind, mode: FileSession.decode32(bytes, at: 4) & 0o7777, size: u64(bytes, 8),
                              modified: timespec(tv_sec: Int(Int64(bitPattern: u64(bytes, 16))), tv_nsec: Int(nanoseconds)),
                              fileID: u64(bytes, 32))
        }

        static func encode64(_ value: UInt64) -> Data { withUnsafeBytes(of: value.littleEndian) { Data($0) } }
    }

    private final class Drop {
        let url: URL
        let name: String
        let created: timespec
        let scoped: Bool
        let staging: URL
        /// The guest unlinked or renamed away the dropped name; the Mac file stays.
        var hidden = false
        init(url: URL, name: String, created: timespec, scoped: Bool, staging: URL) {
            self.url = url; self.name = name; self.created = created; self.scoped = scoped; self.staging = staging
        }
    }
    /// A name inside a drop's directory, resolved for one request.
    private enum Target {
        case directory(Drop)
        case original(Drop)
        case staged(Drop, URL)
        var drop: Drop {
            switch self { case .directory(let drop), .original(let drop), .staged(let drop, _): return drop }
        }
        var path: String? {
            switch self {
            case .directory: return nil
            case .original(let drop): return drop.url.path
            case .staged(_, let url): return url.path
            }
        }
    }
    /// Closed only once released and no request still uses it: a descriptor
    /// number must never be reused while a request could still reach it.
    private final class OpenFile {
        let descriptor: Int32
        let path: String
        let accessMode: Int32
        var users = 0
        var released = false
        init(_ descriptor: Int32, path: String, accessMode: Int32) {
            self.descriptor = descriptor; self.path = path; self.accessMode = accessMode
        }
    }
    /// A guest lock owner's POSIX locks on one file, as OFD locks on one descriptor.
    private struct LockKey: Hashable { let device: Int32; let inode: UInt64; let owner: UInt64 }

    private let lock = NSLock()
    private let stagingRoot: URL
    private var drops: [String: Drop] = [:]
    private var handles: [UInt64: OpenFile] = [:]
    private var lockFiles: [LockKey: Int32] = [:]
    private var nextHandle: UInt64 = 1
    private var session: FileSession?
    private var mount: String?

    /// Files a guest creates next to a drop are kept under `stagingDirectory`
    /// (one directory per drop, removed with it).
    public init(stagingDirectory: URL? = nil) {
        stagingRoot = stagingDirectory ?? FileManager.default.temporaryDirectory
            .appendingPathComponent("nativepipe-drops-" + UUID().uuidString, isDirectory: true)
    }

    deinit {
        detach()
        removeAll()
    }

    /// The guest's mount path, known once a session announced it.
    public var mountPath: String? { lock.withLock { mount } }
    public var isReady: Bool { lock.withLock { session != nil && mount != nil } }
    public var tokens: [String] { lock.withLock { Array(drops.keys) } }

    /// Serves a guest connection, replacing any previous one; its handles close.
    /// `onClose` runs when this session ends, unless a newer one replaced it.
    public func attach(_ connection: SocketConnection, onClose: (@Sendable (Error) -> Void)? = nil) {
        let current = SessionBox()
        let session = FileSession(
            connection: connection, role: .host,
            onRequest: { [weak self] stream, opcode, body in
                guard let self else { stream.cancel(); return }
                Task { await self.serve(stream, opcode, body) }
            },
            onNotification: { [weak self] opcode, body in self?.notified(opcode, body) },
            onClose: { [weak self] error in
                guard let self else { return }
                let ended: Bool = self.lock.withLock {
                    guard let session = current.session, self.session === session else { return false }
                    self.session = nil; self.mount = nil
                    return true
                }
                guard ended else { return }
                self.closeHandles()
                onClose?(error)
            })
        current.session = session
        let previous: FileSession? = lock.withLock {
            defer { self.session = session; mount = nil }
            return self.session
        }
        previous?.close()
        closeHandles()
        session.start()
    }

    public func detach() {
        let previous: FileSession? = lock.withLock {
            defer { session = nil; mount = nil }
            return session
        }
        previous?.close()
        closeHandles()
    }

    /// Makes a regular file available to the guest. Returns its guest URL, or
    /// nil when no guest mount is ready or the item is not a regular file.
    public func share(_ url: URL) -> URL? {
        guard url.isFileURL, let mount = mountPath else { return nil }
        let name = url.lastPathComponent
        guard Self.validName(name) else { return nil }
        var info = stat()
        guard stat(url.path, &info) == 0, info.st_mode & S_IFMT == S_IFREG else { return nil }
        let token = UUID().uuidString
        let scoped = url.startAccessingSecurityScopedResource()
        let drop = Drop(url: url, name: name, created: Self.now(), scoped: scoped,
                        staging: stagingRoot.appendingPathComponent(token, isDirectory: true))
        lock.withLock { drops[token] = drop }
        return try? RemoteFileURL.make(mount + "/" + token + "/" + name)
    }

    /// Forgets a drop and deletes the files the guest created beside it.
    public func remove(token: String) {
        guard let drop = lock.withLock({ drops.removeValue(forKey: token) }) else { return }
        forget(drop)
    }

    public func removeAll() {
        let all: [Drop] = lock.withLock {
            defer { drops.removeAll() }
            return Array(drops.values)
        }
        all.forEach(forget)
        try? FileManager.default.removeItem(at: stagingRoot)
    }

    private func forget(_ drop: Drop) {
        if drop.scoped { drop.url.stopAccessingSecurityScopedResource() }
        try? FileManager.default.removeItem(at: drop.staging)
    }

    // MARK: Requests

    private func notified(_ opcode: UInt16, _ body: Data) {
        guard opcode == Notification.mounted, let path = String(data: body, encoding: .utf8),
              path.hasPrefix("/"), !path.contains("\0"), path.utf8.count < 256 else { return }
        lock.withLock { mount = path }
    }

    private func serve(_ stream: FileSessionStream, _ opcode: UInt16, _ body: Data) async {
        let body = Data(body)
        do {
            let reply: Reply
            switch opcode {
            case Opcode.lookup: reply = await lookup(body)
            case Opcode.list: reply = await list(body)
            case Opcode.open: reply = await open(body)
            case Opcode.read: reply = await read(body)
            case Opcode.release: reply = release(body)
            case Opcode.write: reply = await write(body, stream)
            case Opcode.setattr: reply = await setattr(body)
            case Opcode.create: reply = await create(body)
            case Opcode.unlink: reply = await unlink(body)
            case Opcode.rename: reply = await rename(body)
            case Opcode.fsync: reply = await fsync(body)
            case Opcode.flush: reply = flush(body)
            case Opcode.getlk: reply = await getlk(body)
            case Opcode.setlk: reply = await setlk(body, stream)
            default: reply = .status(Status.einval)
            }
            switch reply {
            case .status(let status): try await stream.respond(status: status, end: true)
            case .body(let data): try await stream.respond(body: data, end: true)
            case .data(let data):
                try await stream.respond()
                try await stream.send(data, end: true)
            }
        } catch {
            stream.cancel()
        }
    }

    private enum Reply { case status(UInt32), body(Data), data(Data) }
    private struct StatusError: Error { let status: UInt32; init(_ status: UInt32) { self.status = status } }

    /// Runs file system calls off the cooperative pool; errno becomes a status.
    private static func io(_ work: @escaping @Sendable () throws -> Reply) async -> Reply {
        do { return try await FileTransferLocalIO.perform(work) }
        catch let error as StatusError { return .status(error.status) }
        catch { return .status(Status.eio) }
    }
    private static func check(_ result: Int32) throws {
        if result < 0 { throw StatusError(Status.from(errno)) }
    }

    private func resolve(_ token: String, _ name: String) -> Target? {
        guard let drop = lock.withLock({ drops[token] }) else { return nil }
        if name.isEmpty { return .directory(drop) }
        if name == drop.name { return lock.withLock({ drop.hidden }) ? nil : .original(drop) }
        let url = drop.staging.appendingPathComponent(name)
        var info = stat()
        return lstat(url.path, &info) == 0 && info.st_mode & S_IFMT == S_IFREG ? .staged(drop, url) : nil
    }

    private func lookup(_ body: Data) async -> Reply {
        guard let (token, name) = Self.path(body), let target = resolve(token, name) else { return .status(Status.enoent) }
        guard let path = target.path else { return .body(directoryAttributes(target.drop).encoded()) }
        return await Self.io { .body(try Self.attributes(path).encoded()) }
    }

    private func list(_ body: Data) async -> Reply {
        if body.isEmpty {
            let all = lock.withLock { drops }
            return .data(all.reduce(into: Data()) { $0.append(Self.entry($1.key, directoryAttributes($1.value))) })
        }
        guard body.count == Self.tokenLength, let token = String(data: body, encoding: .ascii),
              let drop = lock.withLock({ drops[token] }) else { return .status(Status.enoent) }
        let hidden = lock.withLock { drop.hidden }
        return await Self.io {
            var entries = Data()
            if !hidden, let attributes = try? Self.attributes(drop.url.path) {
                entries.append(Self.entry(drop.name, attributes))
            }
            let staged = (try? FileManager.default.contentsOfDirectory(atPath: drop.staging.path)) ?? []
            for name in staged.sorted() where Self.validName(name) && name != drop.name {
                if let attributes = try? Self.attributes(drop.staging.appendingPathComponent(name).path, follow: false) {
                    entries.append(Self.entry(name, attributes))
                }
            }
            return .data(entries)
        }
    }

    private func open(_ body: Data) async -> Reply {
        guard body.count >= 4, let (token, name) = Self.path(body.dropFirst(4)) else { return .status(Status.einval) }
        guard let target = resolve(token, name) else { return .status(Status.enoent) }
        guard let path = target.path else { return .status(Status.eisdir) }
        return await openFile(path, flags: Self.openFlags(FileSession.decode32(body, at: 0)), mode: 0)
    }

    private func create(_ body: Data) async -> Reply {
        guard body.count >= 8, let (token, name) = Self.path(body.dropFirst(8)), !name.isEmpty else {
            return .status(Status.einval)
        }
        guard let drop = lock.withLock({ drops[token] }) else { return .status(Status.enoent) }
        let linux = FileSession.decode32(body, at: 0)
        let mode = mode_t(FileSession.decode32(body, at: 4) & 0o7777)
        var flags = Self.openFlags(linux) | O_CREAT
        if linux & LinuxOpen.exclusive != 0 { flags |= O_EXCL }
        if name == drop.name {
            // The dropped name is the Mac file. While the guest sees it, this is
            // an open; after the guest removed it, the name starts out empty again.
            let hidden = lock.withLock { drop.hidden }
            if hidden { flags = (flags & ~O_EXCL) | O_TRUNC }
            let reply = await openFile(drop.url.path, flags: flags, mode: mode)
            if case .body = reply { lock.withLock { drop.hidden = false } }
            return reply
        }
        do { try FileManager.default.createDirectory(at: drop.staging, withIntermediateDirectories: true) }
        catch { return .status(Status.eio) }
        return await openFile(drop.staging.appendingPathComponent(name).path, flags: flags | O_NOFOLLOW, mode: mode)
    }

    private func openFile(_ path: String, flags: Int32, mode: mode_t) async -> Reply {
        let opened: Reply = await Self.io {
            let descriptor = Darwin.open(path, flags | O_CLOEXEC | O_NONBLOCK, mode)
            try Self.check(descriptor)
            var info = stat()
            guard fstat(descriptor, &info) == 0, info.st_mode & S_IFMT == S_IFREG else {
                Darwin.close(descriptor); throw StatusError(Status.enoent)
            }
            _ = fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) & ~O_NONBLOCK)
            var reply = Attributes.encode64(UInt64(descriptor))
            reply.append(Self.attributes(info).encoded())
            return .body(reply)
        }
        guard case .body(var reply) = opened else { return opened }
        let descriptor = Int32(truncatingIfNeeded: u64(reply, 0))
        let handle: UInt64? = lock.withLock {
            // A handle belongs to the session that opened it.
            guard session != nil else { return nil }
            let handle = nextHandle; nextHandle += 1
            handles[handle] = OpenFile(descriptor, path: path, accessMode: flags & O_ACCMODE)
            return handle
        }
        guard let handle else { Darwin.close(descriptor); return .status(Status.eio) }
        reply.replaceSubrange(0..<8, with: Attributes.encode64(handle))
        return .body(reply)
    }

    private func read(_ body: Data) async -> Reply {
        guard body.count == 20 else { return .status(Status.einval) }
        let offset = u64(body, 8), length = Int(FileSession.decode32(body, at: 16))
        guard length <= Self.readMaximum, offset <= Self.lockToEnd - UInt64(length) else { return .status(Status.einval) }
        return await using(u64(body, 0)) { file in
            var data = Data(count: length)
            var total = 0
            while total < length {
                let count = data.withUnsafeMutableBytes {
                    pread(file.descriptor, $0.baseAddress! + total, length - total, off_t(offset) + off_t(total))
                }
                if count < 0 { if errno == EINTR { continue }; throw StatusError(Status.from(errno)) }
                if count == 0 { break }
                total += count
            }
            data.count = total
            return .data(data)
        }
    }

    private func write(_ body: Data, _ stream: FileSessionStream) async -> Reply {
        guard body.count == 16 else { return .status(Status.einval) }
        var bytes = Data()
        do {
            while let message = try await stream.receive() {
                bytes.append(message.body)
                guard bytes.count <= Self.readMaximum else { return .status(Status.einval) }
            }
        } catch { return .status(Status.eio) }
        let offset = u64(body, 8), payload = bytes
        guard offset <= Self.lockToEnd - UInt64(payload.count) else { return .status(Status.einval) }
        return await using(u64(body, 0)) { file in
            var total = 0
            while total < payload.count {
                let count = payload.withUnsafeBytes {
                    pwrite(file.descriptor, $0.baseAddress! + total, payload.count - total, off_t(offset) + off_t(total))
                }
                if count < 0 { if errno == EINTR { continue }; throw StatusError(Status.from(errno)) }
                total += count
            }
            return .body(FileSession.encode(UInt32(total)))
        }
    }

    private func setattr(_ body: Data) async -> Reply {
        guard body.count >= Self.setattrSize, let (token, name) = Self.path(body.dropFirst(Self.setattrSize)),
              !name.isEmpty else { return .status(Status.einval) }
        let valid = FileSession.decode32(body, at: 0)
        guard valid & (Set.uid | Set.gid) == 0 else { return .status(Status.eperm) }
        let mode = mode_t(FileSession.decode32(body, at: 4) & 0o7777), size = u64(body, 8)
        let atime = timespec(tv_sec: Int(Int64(bitPattern: u64(body, 16))), tv_nsec: Int(FileSession.decode32(body, at: 24)))
        let mtime = timespec(tv_sec: Int(Int64(bitPattern: u64(body, 28))), tv_nsec: Int(FileSession.decode32(body, at: 36)))
        @Sendable func apply(_ descriptor: Int32) throws -> Reply {
            if valid & Set.size != 0 {
                guard size <= Self.lockToEnd else { throw StatusError(Status.einval) }
                try Self.check(ftruncate(descriptor, off_t(size)))
            }
            if valid & Set.mode != 0 { try Self.check(fchmod(descriptor, mode)) }
            if valid & (Set.atime | Set.mtime | Set.atimeNow | Set.mtimeNow) != 0 {
                func time(_ set: UInt32, _ now: UInt32, _ value: timespec) -> timespec {
                    valid & now != 0 ? timespec(tv_sec: 0, tv_nsec: Int(UTIME_NOW))
                        : valid & set != 0 ? value : timespec(tv_sec: 0, tv_nsec: Int(UTIME_OMIT))
                }
                var times = [time(Set.atime, Set.atimeNow, atime), time(Set.mtime, Set.mtimeNow, mtime)]
                try Self.check(futimens(descriptor, &times))
            }
            var info = stat()
            try Self.check(fstat(descriptor, &info))
            return .body(Self.attributes(info).encoded())
        }
        if valid & Set.handle != 0 { return await using(u64(body, 40)) { try apply($0.descriptor) } }
        guard let target = resolve(token, name), let path = target.path else { return .status(Status.enoent) }
        return await Self.io {
            // Only a size change needs write access; times and mode follow ownership.
            let descriptor = Darwin.open(path, (valid & Set.size != 0 ? O_WRONLY : O_RDONLY) | O_CLOEXEC | O_NONBLOCK)
            try Self.check(descriptor)
            defer { Darwin.close(descriptor) }
            return try apply(descriptor)
        }
    }

    private func unlink(_ body: Data) async -> Reply {
        guard let (token, name) = Self.path(body), !name.isEmpty, let target = resolve(token, name) else {
            return .status(Status.enoent)
        }
        switch target {
        case .directory: return .status(Status.eisdir)
        case .original(let drop):
            // The guest can drop its name for the file, never the Mac file itself.
            lock.withLock { drop.hidden = true }
            return .status(0)
        case .staged(_, let url):
            let path = url.path
            return await Self.io { try Self.check(Darwin.unlink(path)); return .status(0) }
        }
    }

    private func rename(_ body: Data) async -> Reply {
        guard body.count >= 6 + Self.tokenLength else { return .status(Status.einval) }
        let flags = FileSession.decode32(body, at: 0)
        let oldLength = Int(body[4]) | Int(body[5]) << 8
        guard body.count >= 6 + Self.tokenLength + oldLength,
              let token = String(data: body.subdata(in: 6..<6 + Self.tokenLength), encoding: .ascii),
              let oldName = String(data: body.subdata(in: 6 + Self.tokenLength..<6 + Self.tokenLength + oldLength), encoding: .utf8),
              let newName = String(data: body.subdata(in: 6 + Self.tokenLength + oldLength..<body.count), encoding: .utf8),
              Self.validName(oldName), Self.validName(newName) else { return .status(Status.einval) }
        guard flags & RenameFlag.exchange == 0, flags & ~RenameFlag.noReplace == 0 else { return .status(Status.einval) }
        guard let drop = lock.withLock({ drops[token] }) else { return .status(Status.enoent) }
        guard let source = resolve(token, oldName) else { return .status(Status.enoent) }
        if oldName == newName { return .status(0) }
        let replacing = resolve(token, newName) != nil
        if replacing && flags & RenameFlag.noReplace != 0 { return .status(Status.eexist) }
        switch source {
        case .directory: return .status(Status.einval)
        case .staged(_, let url) where newName == drop.name:
            // Saving over the dropped name replaces the Mac file, as a Mac app's
            // safe save would.
            let reply = await Self.io { try Self.replace(drop.url, with: url); return .status(0) }
            if case .status(0) = reply { lock.withLock { drop.hidden = false } }
            return reply
        case .staged(_, let url):
            let destination = drop.staging.appendingPathComponent(newName).path, origin = url.path
            return await Self.io { try Self.check(Darwin.rename(origin, destination)); return .status(0) }
        case .original:
            // Moving the dropped name aside (a backup, say) keeps the Mac file
            // where it is: the guest's new name gets a copy, the old one is gone.
            let destination = drop.staging.appendingPathComponent(newName), origin = drop.url
            let reply = await Self.io {
                try FileManager.default.createDirectory(at: drop.staging, withIntermediateDirectories: true)
                try Self.copy(origin, to: destination)
                return .status(0)
            }
            if case .status(0) = reply { lock.withLock { drop.hidden = true } }
            return reply
        }
    }

    private func fsync(_ body: Data) async -> Reply {
        guard body.count == 8 else { return .status(Status.einval) }
        return await using(u64(body, 0)) { file in
            try Self.check(Darwin.fsync(file.descriptor)); return .status(0)
        }
    }

    private func flush(_ body: Data) -> Reply {
        guard body.count == 16 else { return .status(Status.einval) }
        let owner = u64(body, 8)
        guard let file = acquire(u64(body, 0)) else { return .status(Status.ebadf) }
        defer { relinquish(file) }
        var info = stat()
        if fstat(file.descriptor, &info) == 0 {
            let key = LockKey(device: info.st_dev, inode: info.st_ino, owner: owner)
            // The owner's POSIX locks end, as with close(2). Unlock explicitly:
            // the descriptor may share its open file with the handle.
            if let descriptor = lock.withLock({ lockFiles.removeValue(forKey: key) }) { Self.unlockAndClose(descriptor) }
        }
        return .status(0)
    }

    // MARK: Locks

    private struct LockRequest {
        let handle: UInt64, owner: UInt64, start: UInt64, end: UInt64, type: UInt32, flags: UInt32
        init?(_ body: Data) {
            guard body.count == DropFileServer.lockSize else { return nil }
            handle = DropFileServer.u64(body, 0); owner = DropFileServer.u64(body, 8)
            start = DropFileServer.u64(body, 16); end = DropFileServer.u64(body, 24)
            type = FileSession.decode32(body, at: 32); flags = FileSession.decode32(body, at: 36)
            guard type <= LinuxLock.unlock, start <= end, start <= DropFileServer.lockToEnd else { return nil }
        }
        var flock: flock {
            Darwin.flock(l_start: off_t(start), l_len: end >= DropFileServer.lockToEnd ? 0 : off_t(end - start + 1),
                         l_pid: 0, l_type: Int16(type == LinuxLock.read ? F_RDLCK : type == LinuxLock.write ? F_WRLCK : F_UNLCK),
                         l_whence: Int16(SEEK_SET))
        }
    }

    private func getlk(_ body: Data) async -> Reply {
        guard let request = LockRequest(body) else { return .status(Status.einval) }
        // flock(2) has no query; FUSE never asks one, but answer "unlocked".
        if request.flags & LockFlag.flock != 0 { return .body(Self.lockReply(nil, request)) }
        return await using(request.handle) { file in
            let descriptor = try self.lockDescriptor(file, owner: request.owner, create: false) ?? file.descriptor
            var query = request.flock
            if query.l_type == Int16(F_UNLCK) { query.l_type = Int16(F_WRLCK) }
            try Self.check(fcntl(descriptor, F_OFD_GETLK, &query))
            return .body(Self.lockReply(query.l_type == Int16(F_UNLCK) ? nil : query, request))
        }
    }

    private func setlk(_ body: Data, _ stream: FileSessionStream) async -> Reply {
        guard let request = LockRequest(body) else { return .status(Status.einval) }
        let wait = request.flags & LockFlag.wait != 0
        while true {
            let reply = await using(request.handle) { file -> Reply in
                if request.flags & LockFlag.flock != 0 {
                    let operation = request.type == LinuxLock.unlock ? LOCK_UN
                        : (request.type == LinuxLock.read ? LOCK_SH : LOCK_EX) | LOCK_NB
                    try Self.check(np_drop_flock(file.descriptor, operation))
                    return .status(0)
                }
                guard let descriptor = try self.lockDescriptor(file, owner: request.owner,
                                                               create: request.type != LinuxLock.unlock) else {
                    return .status(0) // nothing locked by this owner
                }
                var lock = request.flock
                try Self.check(fcntl(descriptor, F_OFD_SETLK, &lock))
                return .status(0)
            }
            guard wait, case .status(let status) = reply, status == Status.eagain else { return reply }
            // Waiting for a Mac program (or another guest owner) to unlock; the
            // guest cancels the stream if its caller is interrupted.
            if stream.isCancelled { return .status(Status.eintr) }
            try? await Task.sleep(nanoseconds: 20_000_000)
        }
    }

    /// The owner's own descriptor for `file`'s inode: OFD locks then follow
    /// POSIX ownership (one owner, any number of opens) instead of per-open.
    private func lockDescriptor(_ file: OpenFile, owner: UInt64, create: Bool) throws -> Int32? {
        var info = stat()
        try Self.check(fstat(file.descriptor, &info))
        let key = LockKey(device: info.st_dev, inode: info.st_ino, owner: owner)
        if let existing = lock.withLock({ lockFiles[key] }) { return existing }
        guard create else { return nil }
        // Reopen by path when that is still the same file; otherwise the open
        // file itself (renamed or replaced since) carries the owner's locks.
        var descriptor = Darwin.open(file.path, file.accessMode | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW)
        var reopened = stat()
        if descriptor >= 0, fstat(descriptor, &reopened) != 0 || reopened.st_dev != info.st_dev || reopened.st_ino != info.st_ino {
            Darwin.close(descriptor); descriptor = -1
        }
        if descriptor < 0 { descriptor = fcntl(file.descriptor, F_DUPFD_CLOEXEC, 0) }
        try Self.check(descriptor)
        let stored: Int32 = lock.withLock {
            if let raced = lockFiles[key] { return raced }
            lockFiles[key] = descriptor
            return descriptor
        }
        if stored != descriptor { Darwin.close(descriptor) }
        return stored
    }

    private static func lockReply(_ conflict: Darwin.flock?, _ request: LockRequest) -> Data {
        guard let conflict else {
            return encode64(request.start) + encode64(request.end) + FileSession.encode(LinuxLock.unlock) + FileSession.encode(0)
        }
        let start = UInt64(max(conflict.l_start, 0))
        let end = conflict.l_len == 0 ? lockToEnd : start + UInt64(conflict.l_len) - 1
        let type = conflict.l_type == Int16(F_RDLCK) ? LinuxLock.read : LinuxLock.write
        return encode64(start) + encode64(end) + FileSession.encode(type) + FileSession.encode(0)
    }

    // MARK: Handles

    private func acquire(_ handle: UInt64) -> OpenFile? {
        lock.withLock {
            let file = handles[handle]
            file?.users += 1
            return file
        }
    }
    private func relinquish(_ file: OpenFile) {
        let close: Bool = lock.withLock {
            file.users -= 1
            return file.released && file.users == 0
        }
        if close { Darwin.close(file.descriptor) }
    }
    private func using(_ handle: UInt64, _ work: @escaping @Sendable (OpenFile) throws -> Reply) async -> Reply {
        guard let file = acquire(handle) else { return .status(Status.ebadf) }
        let reply = await Self.io { try work(file) }
        relinquish(file)
        return reply
    }

    private func release(_ body: Data) -> Reply {
        guard body.count == 8 else { return .status(Status.einval) }
        guard let file = lock.withLock({ handles.removeValue(forKey: u64(body, 0)) }) else { return .status(Status.ebadf) }
        releaseFile(file)
        return .status(0)
    }

    private func releaseFile(_ file: OpenFile) {
        let close: Bool = lock.withLock {
            file.released = true
            return file.users == 0
        }
        if close { Darwin.close(file.descriptor) }
    }

    /// The session ended: every open file and every lock it held goes.
    private func closeHandles() {
        let (files, locks): ([OpenFile], [Int32]) = lock.withLock {
            defer { handles.removeAll(); lockFiles.removeAll() }
            return (Array(handles.values), Array(lockFiles.values))
        }
        locks.forEach(Self.unlockAndClose)
        files.forEach(releaseFile)
    }

    private static func unlockAndClose(_ descriptor: Int32) {
        var everything = Darwin.flock(l_start: 0, l_len: 0, l_pid: 0, l_type: Int16(F_UNLCK), l_whence: Int16(SEEK_SET))
        _ = fcntl(descriptor, F_OFD_SETLK, &everything)
        Darwin.close(descriptor)
    }

    // MARK: Helpers

    private func directoryAttributes(_ drop: Drop) -> Attributes {
        Attributes(kind: .directory, mode: 0o755, size: 0, modified: drop.created, fileID: 0)
    }

    private static func attributes(_ path: String, follow: Bool = true) throws -> Attributes {
        var info = stat()
        try check(follow ? stat(path, &info) : lstat(path, &info))
        guard info.st_mode & S_IFMT == S_IFREG else { throw StatusError(Status.enoent) }
        return attributes(info)
    }

    private static func attributes(_ info: stat) -> Attributes {
        Attributes(kind: .file, mode: UInt32(info.st_mode) & 0o7777, size: UInt64(max(info.st_size, 0)),
                   modified: info.st_mtimespec, fileID: UInt64(info.st_ino))
    }

    /// Replaces the Mac file's contents by `staged`, keeping the Mac file's
    /// name and attributes the way a Mac app's safe save does.
    private static func replace(_ original: URL, with staged: URL) throws {
        var info = stat()
        if stat(original.path, &info) != 0 && errno == ENOENT {
            // Deleted on the Mac meanwhile: the save recreates it.
            try check(Darwin.rename(staged.path, original.path))
            return
        }
        do {
            _ = try FileManager.default.replaceItemAt(original, withItemAt: staged, options: [])
        } catch {
            // Without permission to replace the item, write it in place.
            let input = Darwin.open(staged.path, O_RDONLY | O_CLOEXEC)
            try check(input)
            defer { Darwin.close(input) }
            let output = Darwin.open(original.path, O_WRONLY | O_TRUNC | O_CLOEXEC)
            try check(output)
            defer { Darwin.close(output) }
            try check(fcopyfile(input, output, nil, copyfile_flags_t(COPYFILE_DATA)))
            _ = Darwin.unlink(staged.path)
        }
    }

    private static func copy(_ source: URL, to destination: URL) throws {
        _ = Darwin.unlink(destination.path)
        if copyfile(source.path, destination.path, nil, copyfile_flags_t(COPYFILE_CLONE | COPYFILE_DATA | COPYFILE_STAT)) != 0 {
            throw StatusError(Status.from(errno))
        }
    }

    private static func openFlags(_ linux: UInt32) -> Int32 {
        var flags: Int32
        switch linux & LinuxOpen.accessMode {
        case 1: flags = O_WRONLY
        case 2: flags = O_RDWR
        default: flags = O_RDONLY
        }
        if linux & LinuxOpen.truncate != 0 { flags |= O_TRUNC }
        if linux & LinuxOpen.append != 0 { flags |= O_APPEND }
        return flags
    }

    private static func entry(_ name: String, _ attributes: Attributes) -> Data {
        let bytes = Data(name.utf8)
        var entry = Data([UInt8(bytes.count & 0xff), UInt8(bytes.count >> 8)])
        entry.append(bytes)
        entry.append(attributes.encoded())
        return entry
    }

    private static func path(_ body: Data) -> (String, String)? {
        let bytes = Data(body)
        guard bytes.count >= tokenLength,
              let token = String(data: bytes.prefix(tokenLength), encoding: .ascii),
              let name = String(data: bytes.dropFirst(tokenLength), encoding: .utf8) else { return nil }
        guard name.isEmpty || validName(name) else { return nil }
        return (token, name)
    }

    static func validName(_ name: String) -> Bool {
        !name.isEmpty && name.utf8.count <= nameMaximum && name != "." && name != ".." &&
            !name.contains("/") && !name.contains("\0")
    }

    static func u64(_ data: Data, _ offset: Int) -> UInt64 {
        data.withUnsafeBytes { UInt64(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: UInt64.self)) }
    }
    private func u64(_ data: Data, _ offset: Int) -> UInt64 { Self.u64(data, offset) }
    private static func encode64(_ value: UInt64) -> Data { Attributes.encode64(value) }

    private static func now() -> timespec {
        var now = timespec()
        clock_gettime(CLOCK_REALTIME, &now)
        return now
    }

    private final class SessionBox: @unchecked Sendable { weak var session: FileSession? }

    /// Wire statuses are Linux errno values.
    public enum Status {
        public static let eperm: UInt32 = 1, enoent: UInt32 = 2, eintr: UInt32 = 4, eio: UInt32 = 5
        public static let ebadf: UInt32 = 9, eagain: UInt32 = 11, eacces: UInt32 = 13, eexist: UInt32 = 17
        public static let eisdir: UInt32 = 21, einval: UInt32 = 22
        public static func from(_ code: Int32) -> UInt32 {
            switch code {
            case EPERM: return eperm
            case ENOENT: return enoent
            case EINTR: return eintr
            case EBADF: return ebadf
            case EAGAIN: return eagain
            case EACCES: return eacces
            case EEXIST: return eexist
            case EXDEV: return 18
            case ENOTDIR: return 20
            case EISDIR: return eisdir
            case EINVAL: return einval
            case ENFILE: return 23
            case EMFILE: return 24
            case EFBIG: return 27
            case ENOSPC: return 28
            case EROFS: return 30
            case EDEADLK: return 35
            case ENAMETOOLONG: return 36
            case ENOLCK: return 37
            case ENOTEMPTY: return 39
            case ELOOP: return 40
            case ENOTSUP: return 95
            case ESTALE: return 116
            case EDQUOT: return 122
            default: return eio
            }
        }
    }
}

private extension DropFileServer.Attributes {
    static func u64(_ data: Data, _ offset: Int) -> UInt64 { DropFileServer.u64(data, offset) }
}
