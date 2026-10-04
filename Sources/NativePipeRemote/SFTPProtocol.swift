import Foundation
import Darwin
import NativePipeStrings

/// The framing limit applies before allocating any server-controlled payload.
enum SFTPWire {
    static let maximumPacket = 1_048_576
    static let chunkSize = 32_768
    static func packet(_ payload: Data) throws -> Data {
        guard !payload.isEmpty, payload.count <= maximumPacket else { throw SFTPFailure.protocolError("Invalid packet length.") }
        var result = Data(); result.sftpUInt32(UInt32(payload.count)); result.append(payload); return result
    }
}

enum SFTPFailure: LocalizedError {
    case protocolError(String), status(UInt32, String), message(String)
    var errorDescription: String? {
        switch self {
        case .protocolError(let value): return NPText("Invalid SFTP response: %@", value)
        case .status(_, let value), .message(let value): return value
        }
    }
}

extension Data {
    mutating func sftpUInt32(_ value: UInt32) {
        append(UInt8(truncatingIfNeeded: value >> 24)); append(UInt8(truncatingIfNeeded: value >> 16))
        append(UInt8(truncatingIfNeeded: value >> 8)); append(UInt8(truncatingIfNeeded: value))
    }
    mutating func sftpUInt64(_ value: UInt64) { sftpUInt32(UInt32(truncatingIfNeeded: value >> 32)); sftpUInt32(UInt32(truncatingIfNeeded: value)) }
    mutating func sftpString(_ value: String) { sftpBytes(Data(value.utf8)) }
    mutating func sftpBytes(_ value: Data) { sftpUInt32(UInt32(value.count)); append(value) }
}

struct SFTPReader {
    private let bytes: [UInt8]
    private(set) var offset = 0
    init(_ data: Data) { bytes = Array(data) }
    var remaining: Int { bytes.count - offset }
    mutating func byte() throws -> UInt8 {
        guard remaining >= 1 else { throw SFTPFailure.protocolError("Truncated packet.") }
        defer { offset += 1 }; return bytes[offset]
    }
    mutating func uint32() throws -> UInt32 {
        guard remaining >= 4 else { throw SFTPFailure.protocolError("Truncated integer.") }
        var value: UInt32 = 0
        for _ in 0..<4 { value = (value << 8) | UInt32(try byte()) }; return value
    }
    mutating func uint64() throws -> UInt64 { let high = try uint32(); return UInt64(high) << 32 | UInt64(try uint32()) }
    mutating func data() throws -> Data {
        let count = try uint32()
        guard count <= SFTPWire.maximumPacket, Int(count) <= remaining else { throw SFTPFailure.protocolError("Truncated string.") }
        let start = offset; offset += Int(count); return Data(bytes[start..<offset])
    }
    mutating func string() throws -> String {
        guard let value = String(data: try data(), encoding: .utf8), !value.contains("\0") else {
            throw SFTPFailure.protocolError("Invalid UTF-8 filename or text.")
        }
        return value
    }
    mutating func attributes() throws -> SFTPAttributes {
        let flags = try uint32()
        guard flags & ~UInt32(0x8000000f) == 0 else { throw SFTPFailure.protocolError("Unknown attribute flags.") }
        var result = SFTPAttributes()
        if flags & 1 != 0 { result.size = try uint64() }
        if flags & 2 != 0 { _ = try uint32(); _ = try uint32() }
        if flags & 4 != 0 { result.permissions = try uint32() }
        if flags & 8 != 0 {
            result.accessed = Date(timeIntervalSince1970: Double(try uint32()))
            result.modified = Date(timeIntervalSince1970: Double(try uint32()))
        }
        if flags & 0x80000000 != 0 {
            let count = try uint32()
            guard count <= 1024, Int(count) <= remaining / 8 else { throw SFTPFailure.protocolError("Too many extended attributes.") }
            for _ in 0..<count { _ = try data(); _ = try data() }
        }
        return result
    }
    func finish() throws { guard remaining == 0 else { throw SFTPFailure.protocolError("Unexpected packet data.") } }
}

struct SFTPAttributes {
    var size: UInt64?
    var permissions: UInt32?
    var modified: Date?
    var accessed: Date?
    var hasKind: Bool { permissions.map { $0 & 0o170000 != 0 } == true }
    var kind: SFTPFileEntry.Kind {
        switch (permissions ?? 0) & 0o170000 {
        case 0o100000: return .file
        case 0o040000: return .directory
        case 0o120000: return .symbolicLink
        default: return .other
        }
    }
}

/// Implementations exchange full framed packets. Tests use the same parser and
/// request flow without starting SSH or requiring a remote account.
protocol SFTPTransport: AnyObject, Sendable {
    func start() throws
    func write(_ data: Data) throws
    func readPacket() throws -> Data
    func checkCancellation() throws
    func beginPublication() throws
    func endPublication()
    func cancel()
    func close()
}

extension SFTPTransport {
    func beginPublication() throws { try checkCancellation() }
    func endPublication() { }
}

final class SFTPSSHTransport: SFTPTransport, @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false
    private var cancellationRequested = false
    private var publishing = false
    private var closed = false
    private var launched = false
    private var process: Process?
    private var diagnostic = Data()
    private let input = Pipe(), output = Pipe(), errorOutput = Pipe()
    private let executable: URL
    private let arguments: [String]
    private let environment: [String: String]
    private let authenticationDirectory: URL
    private let timeout: TimeInterval
    init(executable: URL = URL(fileURLWithPath: "/usr/bin/ssh"), arguments: [String], environment: [String: String],
         authenticationDirectory: URL, timeout: TimeInterval = 45) {
        self.executable = executable; self.arguments = arguments; self.environment = environment
        self.authenticationDirectory = authenticationDirectory; self.timeout = timeout
    }
    func start() throws {
        try checkCancellation()
        let child = Process()
        child.executableURL = executable; child.arguments = arguments; child.environment = environment
        child.standardInput = input; child.standardOutput = output; child.standardError = errorOutput
        lock.lock(); process = child; lock.unlock()
        try child.run()
        lock.lock(); launched = true; let stop = cancelled; lock.unlock()
        try input.fileHandleForReading.close(); try output.fileHandleForWriting.close(); try errorOutput.fileHandleForWriting.close()
        for fd in [input.fileHandleForWriting.fileDescriptor, output.fileHandleForReading.fileDescriptor, errorOutput.fileHandleForReading.fileDescriptor] {
            guard fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0 else { throw POSIXError(.EIO) }
        }
        guard fcntl(input.fileHandleForWriting.fileDescriptor, F_SETNOSIGPIPE, 1) == 0 else { throw POSIXError(.EIO) }
        if stop { stopProcess(child) }
        try checkCancellation()
    }
    func checkCancellation() throws {
        lock.lock(); let stop = cancelled; lock.unlock()
        if stop { throw CancellationError() }
    }
    func beginPublication() throws {
        lock.lock(); defer { lock.unlock() }
        guard !cancelled, !cancellationRequested else { throw CancellationError() }
        publishing = true
    }
    func endPublication() {
        lock.lock(); publishing = false; let stop = cancellationRequested; lock.unlock()
        if stop { cancel() }
    }
    private var responseTimeout: TimeInterval {
        lock.lock(); let protected = publishing; lock.unlock()
        // Publication contains only small rename requests. Keep deferred
        // cancellation bounded even if the server stops answering mid-replace.
        return protected ? min(timeout, 5) : timeout
    }
    private func drainDiagnostics() {
        var bytes = [UInt8](repeating: 0, count: 4096)
        while true {
            let count = Darwin.read(errorOutput.fileHandleForReading.fileDescriptor, &bytes, bytes.count)
            if count <= 0 { break }
            diagnostic.append(contentsOf: bytes.prefix(count))
            if diagnostic.count > 8192 { diagnostic = diagnostic.suffix(8192) }
        }
    }
    private func connectionError() -> Error {
        do { try checkCancellation() } catch { return error }
        drainDiagnostics()
        let message = String(decoding: diagnostic, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        if message.contains("NATIVEPIPE AUTH CANCELLED") { return CancellationError() }
        return SFTPFailure.message(message.isEmpty ? NPText("The SFTP connection closed unexpectedly.") : message)
    }
    private func awaitReady(_ fd: Int32, events: Int16, deadline: TimeInterval) throws {
        while true {
            try checkCancellation(); drainDiagnostics()
            if diagnostic.range(of: Data("NATIVEPIPE AUTH CANCELLED".utf8)) != nil
                || !FileManager.default.fileExists(atPath: authenticationDirectory.path) {
                cancel(); try checkCancellation()
            }
            if ProcessInfo.processInfo.systemUptime >= deadline { throw SFTPFailure.message(NPText("The SFTP server did not respond in time.")) }
            var descriptor = pollfd(fd: fd, events: events, revents: 0)
            let result = poll(&descriptor, 1, 100)
            if result < 0 {
                if errno == EINTR { continue }; throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            if result > 0 {
                if descriptor.revents & events != 0 { return }
                if descriptor.revents & Int16(POLLHUP | POLLERR | POLLNVAL) != 0 { throw connectionError() }
            }
        }
    }
    func write(_ data: Data) throws {
        let deadline = ProcessInfo.processInfo.systemUptime + responseTimeout
        try data.withUnsafeBytes { bytes in
            var offset = 0
            while offset < data.count {
                try awaitReady(input.fileHandleForWriting.fileDescriptor, events: Int16(POLLOUT), deadline: deadline)
                let count = Darwin.write(input.fileHandleForWriting.fileDescriptor, bytes.baseAddress!.advanced(by: offset), data.count - offset)
                if count > 0 { offset += count }
                else if count < 0, errno == EINTR || errno == EAGAIN { continue }
                else { throw connectionError() }
            }
        }
    }
    private func readExactly(_ count: Int, deadline: TimeInterval) throws -> Data {
        var result = Data(count: count)
        try result.withUnsafeMutableBytes { bytes in
            var offset = 0
            while offset < count {
                try awaitReady(output.fileHandleForReading.fileDescriptor, events: Int16(POLLIN), deadline: deadline)
                let amount = Darwin.read(output.fileHandleForReading.fileDescriptor, bytes.baseAddress!.advanced(by: offset), count - offset)
                if amount > 0 { offset += amount }
                else if amount < 0, errno == EINTR || errno == EAGAIN { continue }
                else { throw connectionError() }
            }
        }
        return result
    }
    func readPacket() throws -> Data {
        let deadline = ProcessInfo.processInfo.systemUptime + responseTimeout
        var header = SFTPReader(try readExactly(4, deadline: deadline))
        let count = try header.uint32()
        guard count > 0, count <= SFTPWire.maximumPacket else { throw SFTPFailure.protocolError("Invalid packet length.") }
        return try readExactly(Int(count), deadline: deadline)
    }
    private func stopProcess(_ child: Process) {
        if child.isRunning { child.terminate() }
        DispatchQueue.global().asyncAfter(deadline: .now() + 1) {
            if child.isRunning { kill(child.processIdentifier, SIGKILL) }
        }
    }
    func cancel() {
        lock.lock(); cancellationRequested = true
        if publishing { lock.unlock(); return }
        cancelled = true; let child = launched ? process : nil; lock.unlock()
        try? FileManager.default.removeItem(at: authenticationDirectory)
        if let child { stopProcess(child) }
    }
    func close() {
        lock.lock()
        if closed { lock.unlock(); return }
        closed = true; let child = launched ? process : nil; lock.unlock()
        for handle in [input.fileHandleForReading, input.fileHandleForWriting, output.fileHandleForReading,
                       output.fileHandleForWriting, errorOutput.fileHandleForReading, errorOutput.fileHandleForWriting] { try? handle.close() }
        try? FileManager.default.removeItem(at: authenticationDirectory)
        if let child { stopProcess(child) }
    }
    deinit { close() }
}

final class SFTPConnection {
    let transport: SFTPTransport
    private var nextID: UInt32 = 1
    private var outstanding = Set<UInt32>()
    private var buffered: [UInt32: (UInt8, SFTPReader)] = [:]
    private var home: String?
    private(set) var extensions: [String: String] = [:]
    init(transport: SFTPTransport) throws {
        self.transport = transport
        try transport.start()
        var initPacket = Data([1]); initPacket.sftpUInt32(3)
        try transport.write(SFTPWire.packet(initPacket))
        var reply = SFTPReader(try transport.readPacket())
        guard try reply.byte() == 2, try reply.uint32() == 3 else { throw SFTPFailure.protocolError("SFTP version 3 is required.") }
        while reply.remaining > 0 { let name = try reply.string(); extensions[name] = try reply.string() }
    }
    func checkCancellation() throws { try transport.checkCancellation() }
    func withPublication<Value>(_ operation: () throws -> Value) throws -> Value {
        try transport.beginPublication()
        defer { transport.endPublication() }
        return try operation()
    }
    private func sendRequest(_ type: UInt8, _ payload: Data) throws -> UInt32 {
        try checkCancellation()
        guard outstanding.count < 16 else { throw SFTPFailure.protocolError("Too many outstanding requests.") }
        let id = nextID; nextID &+= 1
        var packet = Data([type]); packet.sftpUInt32(id); packet.append(payload)
        try transport.write(SFTPWire.packet(packet))
        outstanding.insert(id); return id
    }
    private func receiveReply(_ id: UInt32) throws -> (UInt8, SFTPReader) {
        try checkCancellation()
        guard outstanding.contains(id) else { throw SFTPFailure.protocolError("Unknown request identifier.") }
        while buffered[id] == nil {
            var reply = SFTPReader(try transport.readPacket())
            let kind = try reply.byte(), replyID = try reply.uint32()
            guard outstanding.contains(replyID), buffered[replyID] == nil else {
                throw SFTPFailure.protocolError("Mismatched or duplicate request identifier.")
            }
            buffered[replyID] = (kind, reply)
        }
        outstanding.remove(id); return buffered.removeValue(forKey: id)!
    }
    private func request(_ type: UInt8, _ payload: Data) throws -> (UInt8, SFTPReader) {
        try receiveReply(sendRequest(type, payload))
    }
    private func status(_ reader: inout SFTPReader) throws -> UInt32 {
        let code = try reader.uint32(), message = try reader.string(); _ = try reader.string(); try reader.finish()
        if code != 0 && code != 1 {
            throw SFTPFailure.status(code, message.isEmpty ? NPText("The SFTP server rejected the file operation (%@).", String(code)) : message)
        }
        return code
    }
    private func success(_ reply: (UInt8, SFTPReader)) throws {
        var reader = reply.1
        guard reply.0 == 101, try status(&reader) == 0 else { throw SFTPFailure.protocolError("Expected successful status.") }
    }
    private func nameRequest(_ type: UInt8, path: String) throws -> [(String, SFTPAttributes)]? {
        var payload = Data(); payload.sftpString(path)
        return try directoryNames(request(type, payload))
    }
    private func directoryNames(_ reply: (UInt8, SFTPReader)) throws -> [(String, SFTPAttributes)]? {
        var reader = reply.1
        if reply.0 == 101 {
            guard try status(&reader) == 1 else { throw SFTPFailure.protocolError("Expected directory names.") }; return nil
        }
        guard reply.0 == 104 else { throw SFTPFailure.protocolError("Expected directory names.") }
        let count = try reader.uint32()
        guard count <= 100_000, Int(count) <= reader.remaining / 12 else { throw SFTPFailure.protocolError("Invalid directory entry count.") }
        var result: [(String, SFTPAttributes)] = []
        for _ in 0..<count {
            let name = try reader.string(); _ = try reader.data()
            result.append((name, try reader.attributes()))
        }
        try reader.finish(); return result
    }
    func realpath(_ path: String) throws -> String {
        let names = try nameRequest(16, path: path)
        guard let names, names.count == 1, let value = names.first?.0, value.hasPrefix("/"), !value.contains("\0") else {
            throw SFTPFailure.protocolError("Expected one absolute canonical path.")
        }
        return value
    }
    func loginHome() throws -> String {
        if let home { return home }
        let value = try realpath(".")
        home = value
        return value
    }
    func attributes(_ path: String) throws -> SFTPAttributes {
        var payload = Data(); payload.sftpString(path)
        let reply = try request(7, payload); var reader = reply.1
        if reply.0 == 101 { _ = try status(&reader); throw SFTPFailure.protocolError("Expected file attributes.") }
        guard reply.0 == 105 else { throw SFTPFailure.protocolError("Expected file attributes.") }
        let result = try reader.attributes(); try reader.finish(); return result
    }
    func exists(_ path: String) throws -> SFTPAttributes? {
        do { return try attributes(path) }
        catch SFTPFailure.status(let code, _) where code == 2 { return nil }
    }
    private func handle(_ reply: (UInt8, SFTPReader)) throws -> Data {
        var reader = reply.1
        if reply.0 == 101 { _ = try status(&reader); throw SFTPFailure.protocolError("Expected handle.") }
        guard reply.0 == 102 else { throw SFTPFailure.protocolError("Expected handle.") }
        let value = try reader.data(); try reader.finish()
        guard !value.isEmpty, value.count <= 65_536 else { throw SFTPFailure.protocolError("Invalid handle.") }; return value
    }
    func closeHandle(_ handle: Data) throws { var payload = Data(); payload.sftpBytes(handle); try success(request(4, payload)) }
    func entries(_ path: String) throws -> [(String, SFTPAttributes)] {
        var payload = Data(); payload.sftpString(path)
        let directory = try handle(request(11, payload))
        var closeAttempted = false
        defer { if !closeAttempted { try? closeHandle(directory) } }
        var result: [(String, SFTPAttributes)] = []; var seen = Set<Data>(); var received = 0
        var handlePayload = Data(); handlePayload.sftpBytes(directory)
        // READDIR shares the bounded request/reply machinery used by file I/O.
        // Keeping a small window avoids one network round trip for each page.
        var pending = try (0..<8).map { _ in try sendRequest(12, handlePayload) }
        var reachedEnd = false
        while !pending.isEmpty {
            let id = pending.removeFirst()
            if let names = try directoryNames(receiveReply(id)) {
                received += names.count
                guard !names.isEmpty, received <= 100_000 else { throw SFTPFailure.protocolError("Directory listing exceeded its limit.") }
                for (name, attributes) in names {
                    if name == "." || name == ".." { continue }
                    try SFTPPath.validateChild(name)
                    guard seen.insert(Data(name.utf8)).inserted else { throw SFTPFailure.protocolError("Duplicate directory entry.") }
                    result.append((name, attributes))
                }
            } else {
                reachedEnd = true
            }
            // Other requests were already sent before EOF was observed. Drain
            // and validate them, including any names returned out of order,
            // before closing the handle or reusing this connection.
            if !reachedEnd { pending.append(try sendRequest(12, handlePayload)) }
        }
        try checkCancellation()
        let sorted = result.sorted { $0.0.localizedStandardCompare($1.0) == .orderedAscending }
        try checkCancellation()
        // A retained connection may be reused only after CLOSE is acknowledged.
        // Preserve the original error on failed listings, but propagate CLOSE
        // failure on success so the owning session discards this stream.
        closeAttempted = true
        try closeHandle(directory)
        return sorted
    }
    func mkdir(_ path: String) throws {
        var payload = Data(); payload.sftpString(path); payload.sftpUInt32(4); payload.sftpUInt32(0o700)
        try success(request(14, payload))
    }
    func setAttributes(_ path: String, permissions: UInt32?, accessed: Date?, modified: Date?) throws {
        var payload = Data(); payload.sftpString(path)
        var flags: UInt32 = permissions == nil ? 0 : 4
        let modification = modified?.timeIntervalSince1970
        let access = (accessed ?? modified)?.timeIntervalSince1970
        let hasTimes = modification.map { $0 >= 0 && $0 <= Double(UInt32.max) } == true
            && access.map { $0 >= 0 && $0 <= Double(UInt32.max) } == true
        if hasTimes { flags |= 8 }
        if flags == 0 { return }
        payload.sftpUInt32(flags)
        if let permissions { payload.sftpUInt32(permissions & 0o777) }
        if hasTimes { payload.sftpUInt32(UInt32(access!)); payload.sftpUInt32(UInt32(modification!)) }
        try success(request(9, payload))
    }
    func remove(_ path: String, directory: Bool) throws {
        var payload = Data(); payload.sftpString(path); try success(request(directory ? 15 : 13, payload))
    }
    func rename(_ source: String, _ destination: String, replacing: Bool = false) throws {
        var payload = Data()
        if replacing {
            guard extensions["posix-rename@openssh.com"] == "1" else { throw SFTPFailure.message(NPText("This server does not support atomic file replacement.")) }
            payload.sftpString("posix-rename@openssh.com")
        }
        payload.sftpString(source); payload.sftpString(destination)
        try success(request(replacing ? 200 : 18, payload))
    }
    func openFile(_ path: String, writing: Bool) throws -> Data {
        var payload = Data(); payload.sftpString(path); payload.sftpUInt32(writing ? 0x2 | 0x8 | 0x20 : 1)
        payload.sftpUInt32(writing ? 4 : 0)
        if writing { payload.sftpUInt32(0o600) }
        return try handle(request(3, payload))
    }
    func fileAttributes(_ handle: Data) throws -> SFTPAttributes {
        var payload = Data(); payload.sftpBytes(handle)
        let reply = try request(8, payload); var reader = reply.1
        if reply.0 == 101 { _ = try status(&reader); throw SFTPFailure.protocolError("Expected file attributes.") }
        guard reply.0 == 105 else { throw SFTPFailure.protocolError("Expected file attributes.") }
        let result = try reader.attributes(); try reader.finish(); return result
    }
    func writeFile(_ handle: Data, offset: UInt64, data: Data) throws {
        try acknowledgeWrite(enqueueWrite(handle, offset: offset, data: data))
    }
    func enqueueWrite(_ handle: Data, offset: UInt64, data: Data) throws -> UInt32 {
        var payload = Data(); payload.sftpBytes(handle); payload.sftpUInt64(offset); payload.sftpBytes(data)
        return try sendRequest(6, payload)
    }
    func acknowledgeWrite(_ id: UInt32) throws { try success(receiveReply(id)) }
    func readFile(_ handle: Data, offset: UInt64, count: Int) throws -> Data? {
        try receiveRead(enqueueRead(handle, offset: offset, count: count), count: count)
    }
    func enqueueRead(_ handle: Data, offset: UInt64, count: Int) throws -> UInt32 {
        var payload = Data(); payload.sftpBytes(handle); payload.sftpUInt64(offset); payload.sftpUInt32(UInt32(count))
        return try sendRequest(5, payload)
    }
    func receiveRead(_ id: UInt32, count: Int) throws -> Data? {
        let reply = try receiveReply(id); var reader = reply.1
        if reply.0 == 101 { guard try status(&reader) == 1 else { throw SFTPFailure.protocolError("Expected file data.") }; return nil }
        guard reply.0 == 103 else { throw SFTPFailure.protocolError("Expected file data.") }
        let data = try reader.data(); try reader.finish()
        guard !data.isEmpty, data.count <= count else { throw SFTPFailure.protocolError("Invalid file data length.") }; return data
    }
}

enum SFTPPath {
    static func validate(_ path: String) throws {
        guard !path.isEmpty, path.utf8.count < 262_144, !path.contains("\0") else { throw SFTPFailure.message(NPText("Enter a valid remote path.")) }
    }
    static func validateChild(_ name: String) throws {
        guard !name.isEmpty, name != ".", name != "..", !name.contains("/"), !name.contains("\0"), name.utf8.count <= 255 else {
            throw SFTPFailure.protocolError("Unsafe child filename.")
        }
    }
    static func child(_ parent: String, _ name: String) throws -> String {
        try validateChild(name); let value = (parent == "/" ? "/" : parent + "/") + name; try validate(value); return value
    }
    static func destination(_ path: String, connection: SFTPConnection) throws -> String {
        try validate(path)
        guard !path.hasSuffix("/"), path != "/" else { throw SFTPFailure.message(NPText("Choose a file or folder name inside the destination directory.")) }
        let name = (path as NSString).lastPathComponent; try validateChild(name)
        let parent = (path as NSString).deletingLastPathComponent
        return try child(connection.realpath(parent.isEmpty ? "." : parent), name)
    }
}
