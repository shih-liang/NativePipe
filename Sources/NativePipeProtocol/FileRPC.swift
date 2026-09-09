import Foundation
import Darwin
import CNativePipeFileRPC

/// One cancellable operation per connection; identical framing for privileged
/// recovery/guestd and the compositor's unprivileged file worker. The connector
/// selects the endpoint, never a UID supplied by the caller.
public struct FileRPC: Sendable {
    public static let rootPort: UInt32 = 1025
    public static let userPort: UInt32 = 1026
    public typealias Connector = @Sendable () async throws -> FileHandle
    public typealias Progress = @Sendable (_ completed: UInt64, _ expected: UInt64) -> Void
    private let connect: Connector

    public init(connect: @escaping Connector) { self.connect = connect }

    /// An in-memory record over the same DATA/END wire used by file streams.
    /// The receiver chooses a record bound; file upload/download have no such
    /// aggregate bound and never collect a whole file in memory.
    public static func sendRecord(_ data: Data, to handle: FileHandle) throws {
        let socket = Socket(fd: handle.fileDescriptor)
        var offset = 0
        while offset < data.count {
            let end = min(offset + Int(NP_FILE_CHUNK), data.count)
            try socket.send(NP_FILE_DATA, data.subdata(in: offset..<end))
            offset = end
        }
        try socket.send(NP_FILE_END, Socket.encode(UInt64(data.count)))
    }

    public static func receiveRecord(from handle: FileHandle, maximum: Int) throws -> Data {
        guard maximum >= 0 else { throw Failure.protocolError }
        let socket = Socket(fd: handle.fileDescriptor)
        var data = Data()
        while true {
            let frame = try socket.receive()
            if frame.type == NP_FILE_END.rawValue {
                try socket.checkEnd(frame, total: UInt64(data.count))
                return data
            }
            guard frame.type == NP_FILE_DATA.rawValue, !frame.data.isEmpty,
                  frame.data.count <= maximum - data.count else { throw Failure.protocolError }
            data.append(frame.data)
        }
    }

    /// NPAG retains named-artifact negotiation; its payload is this same stream.
    public static func sendStream(_ source: FileHandle, count: UInt64, to socket: FileHandle, framed: Bool = true) throws {
        let result = framed
            ? np_file_send_stream(socket.fileDescriptor, source.fileDescriptor, count)
            : np_file_send_bytes(socket.fileDescriptor, source.fileDescriptor, count)
        guard result == 0 else {
            throw Failure.local(errno)
        }
    }

    public static func receiveStream(from socket: FileHandle, to destination: FileHandle, count: UInt64) throws {
        let wire = Socket(fd: socket.fileDescriptor)
        var received: UInt64 = 0
        while true {
            let frame = try wire.receive()
            if frame.type == NP_FILE_END.rawValue {
                try wire.checkEnd(frame, total: received)
                break
            }
            guard frame.type == NP_FILE_DATA.rawValue, !frame.data.isEmpty,
                  UInt64(frame.data.count) <= count - received else { throw Failure.protocolError }
            try destination.write(contentsOf: frame.data)
            received += UInt64(frame.data.count)
        }
        guard received == count else { throw Failure.protocolError }
    }

    public enum Failure: LocalizedError {
        case protocolError, invalidPath, tooLarge, local(Int32), remote(Int32)
        public var errorDescription: String? {
            switch self {
            case .protocolError: return "The file transfer returned an invalid response."
            case .invalidPath: return "The file path is invalid."
            case .tooLarge: return "Use streaming transfer to read this file."
            case .local(let code): return "File transfer failed: \(String(cString: strerror(code))) (\(code))."
            case .remote(let code):
                // NPFR peers are Linux; Darwin's errno numbering is different.
                let message: String = switch code {
                case 1, 13: "Permission denied"
                case 2: "No such file or directory"
                case 5: "Input/output error"
                case 17: "File already exists"
                case 20: "Not a directory"
                case 21: "Is a directory"
                case 22: "Invalid argument"
                case 27: "File too large"
                case 28: "No space left on device"
                case 30: "Read-only file system"
                case 32: "Broken pipe"
                case 38: "Operation not implemented"
                case 39: "Directory not empty"
                case 40: "Too many symbolic links"
                case 95: "Operation not supported"
                case 104: "Connection reset"
                case 110: "Connection timed out"
                case 125: "Operation cancelled"
                default: "File service error"
                }
                return "Linux file transfer failed: \(message) (\(code))."
            }
        }
    }

    /// Small-file convenience for configuration/procfs. Large file consumers
    /// use download/upload, not this bounded in-memory API.
    public func read(_ path: String, maximum: Int = 8 * 1024 * 1024) async throws -> PathContents {
        try await operation { socket in
            try socket.request(NP_FILE_READ, path: path)
            let info = try socket.metadata(path)
            var contents = PathContents(path: path, isDirectory: info.isDirectory)
            var total: UInt64 = 0
            while true {
                let frame = try socket.receive()
                if frame.type == NP_FILE_END.rawValue {
                    try socket.checkEnd(frame, total: info.isDirectory ? nil : total)
                    return contents
                }
                if info.isDirectory {
                    guard frame.type == NP_FILE_ENTRIES.rawValue else { throw Failure.protocolError }
                    var offset = 0
                    while offset < frame.data.count {
                        guard offset + 3 <= frame.data.count else { throw Failure.protocolError }
                        let type = frame.data[offset]
                        let count = Int(frame.data[offset + 1]) | Int(frame.data[offset + 2]) << 8
                        offset += 3
                        guard count > 0, offset + count <= frame.data.count,
                              let name = String(data: frame.data[offset..<offset + count], encoding: .utf8),
                              name != ".", name != "..", !name.contains("/"), !name.contains("\0")
                        else { throw Failure.protocolError }
                        total += UInt64(count + 3)
                        guard total <= maximum else { throw Failure.tooLarge }
                        contents.entries.append(DirEntry(name: name, fileType: DirEntryType(rawValue: type) ?? .unknown))
                        offset += count
                    }
                } else {
                    guard frame.type == NP_FILE_DATA.rawValue, !frame.data.isEmpty else { throw Failure.protocolError }
                    total += UInt64(frame.data.count)
                    guard total <= maximum else { throw Failure.tooLarge }
                    contents.data.append(frame.data)
                }
            }
        }
    }

    public func stat(_ path: String) async throws -> PathStat {
        try await operation { socket in
            try socket.request(NP_FILE_STAT, path: path)
            return try socket.metadata(path)
        }
    }

    public func createDirectory(_ path: String) async throws {
        try await operation { socket in
            try socket.request(NP_FILE_MKDIR, path: path)
            try socket.checkEnd(socket.receive(), total: nil)
        }
    }

    public func download(_ path: String, to destination: FileHandle,
                         progress: @escaping Progress = { _, _ in }) async throws {
        try await operation { socket in
            try socket.request(NP_FILE_READ, path: path)
            let info = try socket.metadata(path)
            guard info.isRegular else { throw Failure.local(EISDIR) }
            var total: UInt64 = 0
            while true {
                let frame = try socket.receive()
                if frame.type == NP_FILE_END.rawValue {
                    try socket.checkEnd(frame, total: total)
                    return
                }
                guard frame.type == NP_FILE_DATA.rawValue, !frame.data.isEmpty else { throw Failure.protocolError }
                try destination.write(contentsOf: frame.data)
                total += UInt64(frame.data.count)
                progress(total, info.size)
            }
        }
    }

    public func upload(_ source: FileHandle, to path: String, mode: UInt32 = 0o600,
                       replace: Bool = false, progress: @escaping Progress = { _, _ in }) async throws {
        try await operation { socket in
            var st = Darwin.stat()
            guard fstat(source.fileDescriptor, &st) == 0 else { throw Failure.local(errno) }
            guard st.st_mode & S_IFMT == S_IFREG else { throw Failure.local(EINVAL) }
            try socket.request(NP_FILE_WRITE, path: path, mode: mode, replace: replace)
            _ = try socket.metadata(path)
            var total: UInt64 = 0
            while let bytes = try source.read(upToCount: Int(NP_FILE_CHUNK)), !bytes.isEmpty {
                try socket.checkForEarlyFailure()
                try socket.send(NP_FILE_DATA, bytes)
                total += UInt64(bytes.count)
                progress(total, UInt64(max(0, st.st_size)))
            }
            try socket.send(NP_FILE_END, Socket.encode(total))
            try socket.checkEnd(socket.receive(), total: total)
        }
    }

    public func write(_ data: Data, to path: String, mode: UInt32 = 0o600,
                      replace: Bool = true) async throws {
        try await operation { socket in
            try socket.request(NP_FILE_WRITE, path: path, mode: mode, replace: replace)
            _ = try socket.metadata(path)
            var offset = 0
            while offset < data.count {
                try socket.checkForEarlyFailure()
                let end = min(offset + Int(NP_FILE_CHUNK), data.count)
                try socket.send(NP_FILE_DATA, data.subdata(in: offset..<end)); offset = end
            }
            try socket.send(NP_FILE_END, Socket.encode(UInt64(data.count)))
            try socket.checkEnd(socket.receive(), total: UInt64(data.count))
        }
    }

    private func operation<T: Sendable>(_ body: @escaping @Sendable (Socket) throws -> T) async throws -> T {
        let cancellation = Cancellation()
        return try await withTaskCancellationHandler {
            try Task.checkCancellation()
            let handle = try await connect()
            defer { cancellation.finish(); try? handle.close() }
            try cancellation.bind(handle.fileDescriptor)
            return try await withCheckedThrowingContinuation { continuation in
                DispatchQueue.global(qos: .utility).async {
                    do { continuation.resume(returning: try body(Socket(fd: handle.fileDescriptor))) }
                    catch { continuation.resume(throwing: cancellation.isCancelled ? CancellationError() : error) }
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

    private struct Socket {
        let fd: Int32
        struct Frame { var type: UInt32; var data: Data }
        static func encode<T: FixedWidthInteger>(_ value: T) -> Data {
            var value = value.littleEndian; return withUnsafeBytes(of: &value) { Data($0) }
        }
        func send(_ type: np_file_type, _ data: Data, flags: UInt16 = 0) throws {
            let rc = data.withUnsafeBytes { np_file_send(fd, UInt8(type.rawValue), flags, 0, $0.baseAddress, $0.count) }
            guard rc == 0 else { throw Failure.local(errno) }
        }
        func request(_ type: np_file_type, path: String, mode: UInt32 = 0, replace: Bool = false) throws {
            let bytes = Data(path.utf8)
            guard path.hasPrefix("/"), !path.contains("\0"), bytes.count <= 4095 else { throw Failure.invalidPath }
            var body = Self.encode(UInt32(bytes.count)); body.append(bytes)
            if type == NP_FILE_WRITE { body.append(Self.encode(mode)) }
            try send(type, body, flags: replace ? UInt16(NP_FILE_REPLACE) : 0)
        }
        func receive() throws -> Frame {
            let frame = UnsafeMutablePointer<np_file_frame>.allocate(capacity: 1)
            defer { frame.deallocate() }
            guard np_file_receive(fd, frame) == 0 else { throw Failure.local(errno) }
            guard frame.pointee.status == 0 else { throw Failure.remote(Int32(clamping: frame.pointee.status)) }
            let data = Data(bytes: np_file_frame_data(frame), count: Int(frame.pointee.length))
            return Frame(type: UInt32(frame.pointee.type), data: data)
        }
        /// A failed receiver can reply before the upload ends (e.g. ENOSPC).
        /// Observe that reply without a per-chunk acknowledgement roundtrip.
        func checkForEarlyFailure() throws {
            var incoming = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            let ready = poll(&incoming, 1, 0)
            if ready < 0 && errno == EINTR { return }
            guard ready >= 0 else { throw Failure.local(errno) }
            if incoming.revents != 0 {
                _ = try receive() // Nonzero status throws the receiver's error.
                throw Failure.protocolError // Success is only valid after END.
            }
        }
        func integer<T: FixedWidthInteger>(_ data: Data, _ offset: Int, as: T.Type = T.self) -> T {
            data.withUnsafeBytes { T(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: T.self)) }
        }
        func metadata(_ path: String) throws -> PathStat {
            let frame = try receive()
            guard frame.type == NP_FILE_METADATA.rawValue, frame.data.count == 28 else { throw Failure.protocolError }
            let data = frame.data
            return PathStat(path: path, mode: integer(data, 0), uid: integer(data, 4), gid: integer(data, 8),
                            size: integer(data, 12), mtime: integer(data, 20))
        }
        func checkEnd(_ frame: Frame, total: UInt64?) throws {
            guard frame.type == NP_FILE_END.rawValue else { throw Failure.protocolError }
            if let total {
                guard frame.data.count == 8, integer(frame.data, 0, as: UInt64.self) == total else { throw Failure.protocolError }
            } else if !frame.data.isEmpty { throw Failure.protocolError }
        }
    }
}
