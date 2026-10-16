import Foundation
import Darwin

/// Host side of the guest drop filesystem (`common/file_rpc/np_drop_fs.c`).
/// Each dropped file gets a random token; the guest sees it as
/// `<mount>/<token>/<name>`. The host opens the dropped path on demand, as any
/// second Mac app with that file open would: identity is the path, every lookup
/// and open sees the current file, and an open handle keeps the file it opened.
public final class DropFileServer: @unchecked Sendable {
    public enum Opcode {
        public static let lookup: UInt16 = 1, list: UInt16 = 2, open: UInt16 = 3
        public static let read: UInt16 = 4, release: UInt16 = 5
    }
    public enum Notification { public static let mounted: UInt16 = 1 }
    public static let tokenLength = 36
    public static let attributeSize = 40
    public static let readMaximum = 1024 * 1024
    public static let nameMaximum = 255

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
            func u64(_ offset: Int) -> UInt64 {
                bytes.withUnsafeBytes { UInt64(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: UInt64.self)) }
            }
            let nanoseconds = FileSession.decode32(bytes, at: 24)
            guard nanoseconds < 1_000_000_000 else { return nil }
            return Attributes(kind: kind, mode: FileSession.decode32(bytes, at: 4) & 0o7777, size: u64(8),
                              modified: timespec(tv_sec: Int(Int64(bitPattern: u64(16))), tv_nsec: Int(nanoseconds)),
                              fileID: u64(32))
        }

        static func encode64(_ value: UInt64) -> Data { withUnsafeBytes(of: value.littleEndian) { Data($0) } }
    }

    private struct Drop {
        let url: URL
        let name: String
        let created: timespec
        let scoped: Bool
    }
    /// Closed only once released and no read still uses it: a descriptor
    /// number must never be reused while a read could still reach it.
    private final class OpenFile {
        let descriptor: Int32
        var readers = 0
        var released = false
        init(_ descriptor: Int32) { self.descriptor = descriptor }
    }

    private let lock = NSLock()
    private var drops: [String: Drop] = [:]
    private var handles: [UInt64: OpenFile] = [:]
    private var nextHandle: UInt64 = 1
    private var session: FileSession?
    private var mount: String?
    private var onChange: (@Sendable () -> Void)?

    public init() {}

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
        let drop = Drop(url: url, name: name, created: Self.now(), scoped: scoped)
        lock.withLock { drops[token] = drop }
        return try? RemoteFileURL.make(mount + "/" + token + "/" + name)
    }

    public func remove(token: String) {
        let drop: Drop? = lock.withLock { drops.removeValue(forKey: token) }
        if let drop, drop.scoped { drop.url.stopAccessingSecurityScopedResource() }
    }

    public func removeAll() {
        let all: [Drop] = lock.withLock {
            defer { drops.removeAll() }
            return Array(drops.values)
        }
        for drop in all where drop.scoped { drop.url.stopAccessingSecurityScopedResource() }
    }

    // MARK: Requests

    private func notified(_ opcode: UInt16, _ body: Data) {
        guard opcode == Notification.mounted, let path = String(data: body, encoding: .utf8),
              path.hasPrefix("/"), !path.contains("\0"), path.utf8.count < 256 else { return }
        lock.withLock { mount = path }
    }

    private func serve(_ stream: FileSessionStream, _ opcode: UInt16, _ body: Data) async {
        do {
            switch opcode {
            case Opcode.lookup: try await lookup(stream, body)
            case Opcode.list: try await list(stream, body)
            case Opcode.open: try await open(stream, body)
            case Opcode.read: try await read(stream, body)
            case Opcode.release: try await release(stream, body)
            default: try await stream.respond(status: Status.einval, end: true)
            }
        } catch {
            stream.cancel()
        }
    }

    private func lookup(_ stream: FileSessionStream, _ body: Data) async throws {
        guard let (token, name) = Self.path(body), let drop = lock.withLock({ drops[token] }) else {
            return try await stream.respond(status: Status.enoent, end: true)
        }
        if name.isEmpty {
            return try await stream.respond(body: directoryAttributes(drop).encoded(), end: true)
        }
        guard name == drop.name else { return try await stream.respond(status: Status.enoent, end: true) }
        let result = await Self.attributes(of: drop.url)
        switch result {
        case .success(let attributes): try await stream.respond(body: attributes.encoded(), end: true)
        case .failure(let error): try await stream.respond(status: error.status, end: true)
        }
    }

    private func list(_ stream: FileSessionStream, _ body: Data) async throws {
        var entries = Data()
        if body.isEmpty {
            let all = lock.withLock { drops }
            for (token, drop) in all { entries.append(Self.entry(token, directoryAttributes(drop))) }
        } else {
            guard body.count == Self.tokenLength, let token = String(data: body, encoding: .ascii),
                  let drop = lock.withLock({ drops[token] }) else {
                return try await stream.respond(status: Status.enoent, end: true)
            }
            if case .success(let attributes) = await Self.attributes(of: drop.url) {
                entries.append(Self.entry(drop.name, attributes))
            }
        }
        try await stream.respond()
        try await stream.send(entries, end: true)
    }

    private func open(_ stream: FileSessionStream, _ body: Data) async throws {
        guard let (token, name) = Self.path(body), let drop = lock.withLock({ drops[token] }) else {
            return try await stream.respond(status: Status.enoent, end: true)
        }
        _ = token
        guard !name.isEmpty else { return try await stream.respond(status: Status.eisdir, end: true) }
        guard name == drop.name else { return try await stream.respond(status: Status.enoent, end: true) }
        let path = drop.url.path
        let opened: Result<(Int32, Attributes), StatusError> = (try? await FileTransferLocalIO.perform {
            let descriptor = Darwin.open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)
            guard descriptor >= 0 else { return .failure(StatusError(Status.from(errno))) }
            var info = stat()
            guard fstat(descriptor, &info) == 0, info.st_mode & S_IFMT == S_IFREG else {
                Darwin.close(descriptor); return .failure(StatusError(Status.enoent))
            }
            _ = fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) & ~O_NONBLOCK)
            return .success((descriptor, Self.attributes(info)))
        }) ?? .failure(StatusError(Status.eio))
        switch opened {
        case .failure(let error): try await stream.respond(status: error.status, end: true)
        case .success(let (descriptor, attributes)):
            let handle: UInt64? = lock.withLock {
                // A handle belongs to the session that opened it.
                guard session != nil else { return nil }
                let handle = nextHandle; nextHandle += 1
                handles[handle] = OpenFile(descriptor)
                return handle
            }
            guard let handle else {
                Darwin.close(descriptor)
                return try await stream.respond(status: Status.eio, end: true)
            }
            var reply = Attributes.encode64(handle)
            reply.append(attributes.encoded())
            try await stream.respond(body: reply, end: true)
        }
    }

    private func read(_ stream: FileSessionStream, _ body: Data) async throws {
        guard body.count == 20 else { return try await stream.respond(status: Status.einval, end: true) }
        let bytes = Data(body)
        let handle = bytes.withUnsafeBytes { UInt64(littleEndian: $0.loadUnaligned(as: UInt64.self)) }
        let offset = bytes.withUnsafeBytes { UInt64(littleEndian: $0.loadUnaligned(fromByteOffset: 8, as: UInt64.self)) }
        let length = Int(FileSession.decode32(bytes, at: 16))
        guard length <= Self.readMaximum, offset <= UInt64(Int64.max) - UInt64(length) else {
            return try await stream.respond(status: Status.einval, end: true)
        }
        guard let file: OpenFile = lock.withLock({
            let file = handles[handle]
            file?.readers += 1
            return file
        }) else {
            return try await stream.respond(status: Status.ebadf, end: true)
        }
        let descriptor = file.descriptor
        let data: Result<Data, StatusError> = (try? await FileTransferLocalIO.perform {
            var data = Data(count: length)
            var total = 0
            while total < length {
                let count = data.withUnsafeMutableBytes {
                    pread(descriptor, $0.baseAddress! + total, length - total, off_t(offset) + off_t(total))
                }
                if count < 0 { if errno == EINTR { continue }; return .failure(StatusError(Status.from(errno))) }
                if count == 0 { break }
                total += count
            }
            data.count = total
            return .success(data)
        }) ?? .failure(StatusError(Status.eio))
        finishRead(file)
        switch data {
        case .failure(let error): try await stream.respond(status: error.status, end: true)
        case .success(let data):
            try await stream.respond()
            try await stream.send(data, end: true)
        }
    }

    private func release(_ stream: FileSessionStream, _ body: Data) async throws {
        guard body.count == 8 else { return try await stream.respond(status: Status.einval, end: true) }
        let handle = Data(body).withUnsafeBytes { UInt64(littleEndian: $0.loadUnaligned(as: UInt64.self)) }
        guard let file = lock.withLock({ handles.removeValue(forKey: handle) }) else {
            return try await stream.respond(status: Status.ebadf, end: true)
        }
        releaseFile(file)
        try await stream.respond(end: true)
    }

    private func finishRead(_ file: OpenFile) {
        let close: Bool = lock.withLock {
            file.readers -= 1
            return file.released && file.readers == 0
        }
        if close { Darwin.close(file.descriptor) }
    }

    private func releaseFile(_ file: OpenFile) {
        let close: Bool = lock.withLock {
            file.released = true
            return file.readers == 0
        }
        if close { Darwin.close(file.descriptor) }
    }

    private func closeHandles() {
        let all: [OpenFile] = lock.withLock {
            defer { handles.removeAll() }
            return Array(handles.values)
        }
        all.forEach(releaseFile)
    }

    // MARK: Helpers

    private func directoryAttributes(_ drop: Drop) -> Attributes {
        Attributes(kind: .directory, mode: 0o555, size: 0, modified: drop.created, fileID: 0)
    }

    private static func attributes(of url: URL) async -> Result<Attributes, StatusError> {
        let path = url.path
        return (try? await FileTransferLocalIO.perform { () -> Result<Attributes, StatusError> in
            var info = stat()
            guard stat(path, &info) == 0 else { return .failure(StatusError(Status.from(errno))) }
            guard info.st_mode & S_IFMT == S_IFREG else { return .failure(StatusError(Status.enoent)) }
            return .success(attributes(info))
        }) ?? .failure(StatusError(Status.eio))
    }

    private static func attributes(_ info: stat) -> Attributes {
        Attributes(kind: .file, mode: UInt32(info.st_mode) & 0o7777, size: UInt64(max(info.st_size, 0)),
                   modified: info.st_mtimespec, fileID: UInt64(info.st_ino))
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

    private static func now() -> timespec {
        var now = timespec()
        clock_gettime(CLOCK_REALTIME, &now)
        return now
    }

    private final class SessionBox: @unchecked Sendable { weak var session: FileSession? }

    private struct StatusError: Error { let status: UInt32; init(_ status: UInt32) { self.status = status } }

    /// Wire statuses are Linux errno values.
    public enum Status {
        public static let enoent: UInt32 = 2, eio: UInt32 = 5, ebadf: UInt32 = 9, eacces: UInt32 = 13
        public static let eisdir: UInt32 = 21, einval: UInt32 = 22
        public static func from(_ code: Int32) -> UInt32 {
            switch code {
            case EPERM: return 1
            case ENOENT: return enoent
            case EBADF: return ebadf
            case EACCES: return eacces
            case ENOTDIR: return 20
            case EISDIR: return eisdir
            case EINVAL: return einval
            case ENFILE: return 23
            case EMFILE: return 24
            case EROFS: return 30
            case ENAMETOOLONG: return 36
            case ELOOP: return 40
            case ESTALE: return 116
            default: return eio
            }
        }
    }
}
