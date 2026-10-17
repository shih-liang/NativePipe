import NativePipeStrings
import Foundation
import Darwin
import CNativePipeFileRPC

/// Guest file services: one NPFR session per operation, identical for
/// privileged recovery/guestd and the compositor's unprivileged file worker.
/// The connector selects the endpoint, never a UID supplied by the caller.
/// Closing the session releases every handle the operation opened.
public struct FileRPC: Sendable {
    public static let rootPort = NP_FILE_ROOT_PORT
    public static let userPort = NP_FILE_USER_PORT
    public typealias Connector = @Sendable () async throws -> FileHandle
    public typealias Progress = @Sendable (_ completed: UInt64, _ expected: UInt64) -> Void
    private let connect: Connector
    private static let ioMaximum = Int(NP_FILE_IO_MAX)

    public init(connect: @escaping Connector) { self.connect = connect }

    public enum Failure: LocalizedError {
        case protocolError, invalidPath, tooLarge, sourceChanged(String), local(Int32), remote(Int32)
        public var errorDescription: String? {
            switch self {
            case .protocolError: return NPText("The file transfer returned an invalid response.")
            case .invalidPath: return NPText("The file path is invalid.")
            case .tooLarge: return NPText("Use streaming transfer to read this file.")
            case .sourceChanged(let path): return NPText("The source changed during the transfer: %@.", path)
            case .local(let code): return NPText("File transfer failed: %@ (%@).", String(cString: strerror(code)), String(code))
            case .remote(let code):
                // NPFR peers are Linux; Darwin's errno numbering is different.
                let message: String = switch code {
                case 1, 13: NPText("Permission denied")
                case 2: NPText("No such file or directory")
                case 5: NPText("Input/output error")
                case 17: NPText("File already exists")
                case 20: NPText("Not a directory")
                case 21: NPText("Is a directory")
                case 22: NPText("Invalid argument")
                case 27: NPText("File too large")
                case 28: NPText("No space left on device")
                case 30: NPText("Read-only file system")
                case 32: NPText("Broken pipe")
                case 38: NPText("Operation not implemented")
                case 39: NPText("Directory not empty")
                case 40: NPText("Too many symbolic links")
                case 95: NPText("Operation not supported")
                case 104: NPText("Connection reset")
                case 110: NPText("Connection timed out")
                case 125: NPText("Operation cancelled")
                default: NPText("File service error")
                }
                return NPText("Linux file transfer failed: %@ (%@).", message, String(code))
            }
        }
    }

    /// Small-file convenience for configuration/procfs. Large file consumers
    /// use download/upload, not this bounded in-memory API.
    public func read(_ path: String, maximum: Int = 8 * 1024 * 1024) async throws -> PathContents {
        try await readContents(path, flags: 0, maximum: maximum)
    }
    /// A lazy directory listing pins the same no-symlink root as range reads.
    public func directoryEntries(_ path: String, maximum: Int = 8 * 1024 * 1024) async throws -> [DirEntry] {
        try UserFileRange.validate(RemoteFileURL.make(path))
        return try await readContents(path, flags: NP_FILE_OPEN_DIRECTORY | NP_FILE_NOFOLLOW, maximum: maximum).entries
    }
    private func readContents(_ path: String, flags: UInt32, maximum: Int) async throws -> PathContents {
        guard maximum >= 0 else { throw Failure.protocolError }
        return try await operation { session in
            let (handle, info) = try session.open(path, flags: flags)
            var contents = PathContents(path: path, isDirectory: info.isDirectory)
            var total = 0
            while true {
                let page = try session.read(handle)
                if page.isEmpty { return contents }
                total += page.count
                guard total <= maximum else { throw Failure.tooLarge }
                guard info.isDirectory else { contents.data.append(page); continue }
                var offset = page.startIndex
                while offset < page.endIndex {
                    guard page.endIndex - offset >= 3 else { throw Failure.protocolError }
                    let type = page[offset]
                    let count = Int(page[offset + 1]) | Int(page[offset + 2]) << 8
                    offset += 3
                    guard count > 0, count <= page.endIndex - offset,
                          let name = String(data: page[offset..<offset + count], encoding: .utf8),
                          name != ".", name != "..", !name.contains("/"), !name.contains("\0")
                    else { throw Failure.protocolError }
                    contents.entries.append(DirEntry(name: name, fileType: DirEntryType(rawValue: type) ?? .unknown))
                    offset += count
                }
            }
        }
    }

    public struct DirectoryEntry: Sendable {
        public let name: String
        public let metadata: PathStat
        public init(name: String, metadata: PathStat) { self.name = name; self.metadata = metadata }
    }
    public struct DirectoryListing: Sendable {
        public let path: String
        public let home: String
        public let entries: [DirectoryEntry]
        public init(path: String, home: String, entries: [DirectoryEntry]) {
            self.path = path; self.home = home; self.entries = entries
        }
    }

    /// The listing carries the path, the worker's actual home and lstat for
    /// each name; no request per entry is necessary.
    public func browse(_ path: String? = nil) async throws -> DirectoryListing {
        if let path { try UserFileRange.validate(RemoteFileURL.make(path)) }
        return try await operation { session in
            let flags = NP_FILE_OPEN_DIRECTORY | NP_FILE_OPEN_STAT | NP_FILE_OPEN_HOME | NP_FILE_NOFOLLOW
            let reply = try session.call(NP_FILE_OPEN, Session.encode(flags) + Session.encode(UInt32(0))
                + Session.path(path ?? "", allowEmpty: true)).body
            let base = 8 + Int(NP_FILE_METADATA)
            guard reply.count >= base + 8 else { throw Failure.protocolError }
            let handle: UInt64 = Session.integer(reply, 0)
            let pathLength = Int(Session.integer(reply, base, as: UInt32.self))
            let homeLength = Int(Session.integer(reply, base + 4, as: UInt32.self))
            guard pathLength > 0, pathLength <= 4095, homeLength > 0, homeLength <= 4095,
                  reply.count == base + 8 + pathLength + homeLength,
                  let resolved = String(data: reply.subdata(in: (base + 8)..<(base + 8 + pathLength)), encoding: .utf8),
                  let home = String(data: reply.subdata(in: (base + 8 + pathLength)..<reply.count), encoding: .utf8)
            else { throw Failure.protocolError }
            try UserFileRange.validate(RemoteFileURL.make(resolved)); try UserFileRange.validate(RemoteFileURL.make(home))
            var entries: [DirectoryEntry] = [], names = Set<Data>(), metadataBytes = 0
            while true {
                let page = try session.read(handle)
                if page.isEmpty { return DirectoryListing(path: resolved, home: home, entries: entries) }
                metadataBytes += page.count
                guard metadataBytes <= 64 * 1024 * 1024 else { throw Failure.tooLarge }
                var offset = 0
                while offset < page.count {
                    guard page.count - offset >= 30 else { throw Failure.protocolError }
                    let count = Int(page[offset]) | Int(page[offset + 1]) << 8
                    guard count > 0, count <= 255, count <= page.count - offset - 30 else { throw Failure.protocolError }
                    let bytes = page.subdata(in: (offset + 30)..<(offset + 30 + count))
                    guard names.insert(bytes).inserted, let name = String(data: bytes, encoding: .utf8),
                          name != ".", name != "..", !name.contains("/"), !name.contains("\0") else { throw Failure.protocolError }
                    let child = try RemoteFileURL.appending(name, to: RemoteFileURL.make(resolved, isDirectory: true))
                    entries.append(.init(name: name, metadata: Session.metadata(page, at: offset + 2, path: child.path)))
                    offset += 30 + count
                }
            }
        }
    }

    public struct UploadStaging: Sendable {
        public let path: String
        let token: Data
    }
    public func createUploadStaging(_ path: String) async throws -> UploadStaging {
        try UserFileRange.validate(RemoteFileURL.make(path))
        var token = Data(repeating: 0, count: Int(NP_FILE_STAGING_TOKEN))
        token.withUnsafeMutableBytes { arc4random_buf($0.baseAddress, $0.count) }
        let staging = UploadStaging(path: path, token: token)
        do {
            return try await operation { session in
                _ = try session.call(NP_FILE_CREATE_STAGING, staging.token + Session.path(path))
                return staging
            }
        } catch {
            // The capability is known before CREATE, including when its reply
            // is lost. Only that capability can remove the owned private tree.
            let rpc = self
            await Task.detached(priority: .utility) { try? await rpc.discardStaging(staging) }.value
            throw error
        }
    }
    public func publishStaging(_ staging: UploadStaging, to destination: String) async throws {
        try UserFileRange.validate(RemoteFileURL.make(destination))
        try await operation { session in
            _ = try session.call(NP_FILE_PUBLISH_STAGING,
                                 staging.token + Session.path(destination) + Session.path(staging.path))
        }
    }
    public func discardStaging(_ staging: UploadStaging) async throws {
        try await operation { session in
            _ = try session.call(NP_FILE_DISCARD_STAGING, staging.token + Session.path(staging.path))
        }
    }

    public func stat(_ path: String) async throws -> PathStat {
        try await operation { session in
            let reply = try session.call(NP_FILE_STAT, Session.path(path)).body
            guard reply.count == Int(NP_FILE_METADATA) else { throw Failure.protocolError }
            return Session.metadata(reply, at: 0, path: path)
        }
    }

    /// A pinned-file revision includes inode, device, size, and nanosecond
    /// modification/change times. No file contents are read by this request.
    public func snapshot(_ path: String) async throws -> UserFileMetadata {
        try UserFileRange.validate(RemoteFileURL.make(path))
        return try await operation { session in
            let reply = try session.call(NP_FILE_SNAPSHOT, Session.path(path)).body
            guard reply.count == Int(NP_FILE_METADATA + NP_FILE_REVISION) else { throw Failure.protocolError }
            let mode: UInt32 = Session.integer(reply, 0)
            let kind: UserFileMetadata.Kind = switch mode & 0o170000 {
            case 0o100000: .file
            case 0o040000: .directory
            case 0o120000: .symbolicLink
            default: .other
            }
            let mtime: Int64 = Session.integer(reply, 20)
            return UserFileMetadata(url: try RemoteFileURL.make(path, isDirectory: kind == .directory), kind: kind,
                size: Session.integer(reply, 12), modified: Date(timeIntervalSince1970: Double(mtime)), permissions: mode,
                version: reply.subdata(in: Int(NP_FILE_METADATA)..<reply.count))
        }
    }

    public func readRange(_ path: String, offset: UInt64, length: Int, expectedVersion: Data) async throws -> Data {
        try UserFileRange.validate(RemoteFileURL.make(path), offset: offset, length: length)
        guard expectedVersion.count == Int(NP_FILE_REVISION), length <= Self.ioMaximum else { throw Failure.protocolError }
        do {
            return try await operation { session in
                let data = try session.call(NP_FILE_RANGE, Session.encode(offset) + Session.encode(UInt32(length))
                    + expectedVersion + Session.path(path)).data
                guard data.count <= length else { throw Failure.protocolError }
                return data
            }
        } catch Failure.remote(let code) where code == Int32(NP_FILE_STALE) { throw Failure.sourceChanged(path) }
    }

    public func createDirectory(_ path: String, noFollow: Bool = false) async throws {
        try await operation { session in
            _ = try session.call(NP_FILE_MKDIR, Session.encode(noFollow ? NP_FILE_NOFOLLOW : 0) + Session.path(path))
        }
    }

    public func download(_ path: String, to destination: FileHandle,
                         progress: @escaping Progress = { _, _ in }) async throws {
        try await operation { session in
            let (handle, info) = try session.open(path, flags: 0)
            guard info.isRegular else { throw Failure.local(EISDIR) }
            var total: UInt64 = 0
            while true {
                let bytes = try session.read(handle)
                if bytes.isEmpty { return }
                try destination.write(contentsOf: bytes)
                total += UInt64(bytes.count)
                progress(total, info.size)
            }
        }
    }

    public func upload(_ source: FileHandle, to path: String, mode: UInt32 = 0o600,
                       replace: Bool = false, noFollow: Bool = false, progress: @escaping Progress = { _, _ in }) async throws {
        try await operation { session in
            var st = Darwin.stat()
            guard fstat(source.fileDescriptor, &st) == 0 else { throw Failure.local(errno) }
            guard st.st_mode & S_IFMT == S_IFREG else { throw Failure.local(EINVAL) }
            let handle = try session.create(path, mode: mode, replace: replace, noFollow: noFollow)
            var total: UInt64 = 0
            while let bytes = try source.read(upToCount: Self.ioMaximum), !bytes.isEmpty {
                try session.write(handle, bytes)
                total += UInt64(bytes.count)
                progress(total, UInt64(max(0, st.st_size)))
            }
            try session.close(handle)
        }
    }

    public func write(_ data: Data, to path: String, mode: UInt32 = 0o600,
                      replace: Bool = true) async throws {
        try await operation { session in
            let handle = try session.create(path, mode: mode, replace: replace, noFollow: false)
            var offset = data.startIndex
            while offset < data.endIndex {
                let end = min(offset + Self.ioMaximum, data.endIndex)
                try session.write(handle, data.subdata(in: offset..<end)); offset = end
            }
            try session.close(handle)
        }
    }

    private func operation<T: Sendable>(_ body: @escaping @Sendable (Session) throws -> T) async throws -> T {
        let cancellation = Cancellation()
        return try await withTaskCancellationHandler {
            try Task.checkCancellation()
            let handle = try await connect()
            defer { cancellation.finish(); try? handle.close() }
            try cancellation.bind(handle.fileDescriptor)
            return try await withCheckedThrowingContinuation { continuation in
                DispatchQueue.global(qos: .utility).async {
                    do {
                        let session = try Session(socket: handle.fileDescriptor)
                        defer { session.close() }
                        continuation.resume(returning: try body(session))
                    } catch { continuation.resume(throwing: cancellation.isCancelled ? CancellationError() : error) }
                }
            }
        } onCancel: { cancellation.cancel() }
    }

    /// Cancellation shuts down, but does not close/reuse a descriptor while a
    /// worker still owns it. The operation closes it only after the worker ends.
    private final class Cancellation: @unchecked Sendable {
        private let lock = NSLock()
        private var fd: Int32 = -1
        private var cancelled = false
        var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
        func bind(_ fd: Int32) throws {
            lock.lock(); defer { lock.unlock() }
            guard !cancelled else { throw CancellationError() }
            self.fd = fd
        }
        func cancel() {
            lock.lock(); defer { lock.unlock() }
            cancelled = true
            if fd >= 0 { shutdown(fd, SHUT_RDWR) }
        }
        func finish() {
            lock.lock(); defer { lock.unlock() }
            if fd >= 0 { shutdown(fd, SHUT_RDWR) }; fd = -1
        }
    }

    /// A blocking NPFR session on a duplicate of the operation's socket, so
    /// shutting the original down (cancellation) also ends this session.
    private final class Session {
        private let raw: OpaquePointer
        private let message = UnsafeMutablePointer<np_file2_message>.allocate(capacity: 1)

        init(socket: Int32) throws {
            let duplicate = dup(socket)
            guard duplicate >= 0 else { message.deallocate(); throw Failure.local(errno) }
            guard let raw = np_file2_session_start(duplicate, NP_FILE2_ROLE_HOST, nil) else {
                message.deallocate(); throw Failure.local(errno)
            }
            self.raw = raw
        }
        func close() { np_file2_session_close(raw); message.deallocate() }

        /// One request: its response body and any DATA. A nonzero status
        /// throws the guest's error.
        func call(_ opcode: np_file_opcode, _ body: Data, data: Data? = nil) throws -> (body: Data, data: Data) {
            let stream = body.withUnsafeBytes {
                np_file2_request(raw, UInt16(opcode.rawValue), $0.baseAddress, $0.count, data == nil ? 1 : 0)
            }
            guard let stream else { throw Failure.local(errno) }
            defer { np_file2_stream_release(stream) }
            // A guest that fails early still answers; read its status first.
            var sendError: Int32 = 0
            if let data, data.withUnsafeBytes({ np_file2_send(stream, $0.baseAddress, $0.count, 1) }) != 0 {
                sendError = errno
            }
            guard np_file2_receive(stream, message) == 1 else { throw Failure.local(errno) }
            guard message.pointee.type == UInt8(NP_FILE2_RESPONSE.rawValue) else { throw Failure.protocolError }
            let status = message.pointee.status
            let reply = Data(bytes: np_file2_message_data(message), count: Int(message.pointee.length))
            var received = Data()
            while true {
                let result = np_file2_receive(stream, message)
                if result == 0 { break }
                guard result == 1 else { throw Failure.local(errno) }
                guard message.pointee.type == UInt8(NP_FILE2_DATA.rawValue) else { throw Failure.protocolError }
                received.append(np_file2_message_data(message), count: Int(message.pointee.length))
            }
            guard status == 0 else { throw Failure.remote(Int32(clamping: status)) }
            guard sendError == 0 else { throw Failure.local(sendError) }
            return (reply, received)
        }

        func open(_ path: String, flags: UInt32) throws -> (UInt64, PathStat) {
            let reply = try call(NP_FILE_OPEN, Self.encode(flags) + Self.encode(UInt32(0)) + Self.path(path)).body
            guard reply.count == 8 + Int(NP_FILE_METADATA) else { throw Failure.protocolError }
            return (Self.integer(reply, 0), Self.metadata(reply, at: 8, path: path))
        }
        func create(_ path: String, mode: UInt32, replace: Bool, noFollow: Bool) throws -> UInt64 {
            let flags = NP_FILE_OPEN_WRITE | (replace ? NP_FILE_REPLACE : 0) | (noFollow ? NP_FILE_NOFOLLOW : 0)
            let reply = try call(NP_FILE_OPEN, Self.encode(flags) + Self.encode(mode) + Self.path(path)).body
            guard reply.count == 8 + Int(NP_FILE_METADATA) else { throw Failure.protocolError }
            return Self.integer(reply, 0)
        }
        /// The next page; empty at the end of a file or directory.
        func read(_ handle: UInt64) throws -> Data {
            let data = try call(NP_FILE_READ, Self.encode(handle) + Self.encode(NP_FILE_IO_MAX)).data
            guard data.count <= Int(NP_FILE_IO_MAX) else { throw Failure.protocolError }
            return data
        }
        func write(_ handle: UInt64, _ bytes: Data) throws {
            let reply = try call(NP_FILE_WRITE, Self.encode(handle), data: bytes).body
            guard reply.count == 4, Self.integer(reply, 0, as: UInt32.self) == UInt32(bytes.count) else {
                throw Failure.protocolError
            }
        }
        /// For a written file, reports the guest's chmod, fsync and close.
        func close(_ handle: UInt64) throws { _ = try call(NP_FILE_CLOSE, Self.encode(handle)) }

        static func encode<T: FixedWidthInteger>(_ value: T) -> Data {
            var value = value.littleEndian; return withUnsafeBytes(of: &value) { Data($0) }
        }
        static func path(_ path: String, allowEmpty: Bool = false) throws -> Data {
            let bytes = Data(path.utf8)
            guard path.hasPrefix("/") || (allowEmpty && path.isEmpty), !path.contains("\0"),
                  bytes.count <= Int(NP_FILE_PATH_MAX) else { throw Failure.invalidPath }
            return encode(UInt32(bytes.count)) + bytes
        }
        static func integer<T: FixedWidthInteger>(_ data: Data, _ offset: Int, as: T.Type = T.self) -> T {
            data.withUnsafeBytes { T(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: T.self)) }
        }
        static func metadata(_ data: Data, at offset: Int, path: String) -> PathStat {
            PathStat(path: path, mode: integer(data, offset), uid: integer(data, offset + 4), gid: integer(data, offset + 8),
                     size: integer(data, offset + 12), mtime: integer(data, offset + 20))
        }
    }
}
