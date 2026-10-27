import Foundation
import Darwin

/// NPFR v2: one long-lived, full-duplex connection carrying many concurrent
/// streams, matching `common/file_rpc/np_file_session.c`. Either side opens
/// streams and sends notifications; the role only decides stream ID parity.
///
/// Frames are written one at a time, in order. Each direction of a stream
/// carries at most `Wire.streamDataMaximum` bytes of DATA, which with the
/// inbound stream limit bounds what a peer can make this side buffer.
///
/// The session stays alive until `close()` or a disconnect.
public final class FileSession: @unchecked Sendable {
    public enum Role: UInt8, Sendable { case host = 0, guest = 1 }

    public struct Message: Sendable, Equatable {
        public enum Kind: Sendable, Equatable { case response(status: UInt32), data }
        public var kind: Kind
        public var body: Data
        /// The peer finished its half of the stream with this message.
        public var end: Bool
    }

    public enum Wire {
        public static let version: UInt8 = 2
        public static let headerSize = 16
        public static let frameMaximum = 65536
        /// DATA one direction of a stream may carry; more ends the session.
        public static let streamDataMaximum = 1024 * 1024
        public static let maximumInboundStreams = 64
        /// Frames waiting while the peer is not reading; more drops the peer.
        public static let maximumPendingControl = 65536
        public static let port = NativePipePort.fileSession
        public static let endStream: UInt8 = 1
    }

    enum FrameType: UInt8 {
        case hello = 1, request, response, data, notify, cancel
    }

    /// Runs on the reader; must not block. The stream is cancelled when the
    /// last reference is dropped before both directions finish.
    public typealias RequestHandler = @Sendable (_ stream: FileSessionStream, _ opcode: UInt16, _ body: Data) -> Void
    public typealias NotificationHandler = @Sendable (_ opcode: UInt16, _ body: Data) -> Void
    public typealias CloseHandler = @Sendable (_ error: Error) -> Void

    private let connection: SocketConnection
    let role: Role
    private let onRequest: RequestHandler?
    private let onNotification: NotificationHandler?
    private let onClose: CloseHandler?
    private let lock = NSLock()

    private final class StreamState {
        let id: UInt32
        let responder: Bool
        var responded = false, localEnded = false, remoteEnded = false, cancelled = false
        var sent = 0, received = 0
        var inbound: [Message] = []
        var receiver: CheckedContinuation<Message?, Error>?
        init(id: UInt32, responder: Bool) { self.id = id; self.responder = responder }
        var finished: Bool { localEnded && remoteEnded }
    }
    private struct Frame {
        let bytes: Data
        let done: CheckedContinuation<Void, Error>?
    }

    private var streams: [UInt32: StreamState] = [:]
    private var nextID: UInt32
    private var lastPeerID: UInt32 = 0
    private var inboundStreams = 0
    private var frames: [Frame] = []
    private var writing = false
    private var failure: Error?
    private var started = false

    /// Takes ownership of the connection. Call `start()` once handlers can run.
    public init(connection: SocketConnection, role: Role,
                onRequest: RequestHandler? = nil,
                onNotification: NotificationHandler? = nil,
                onClose: CloseHandler? = nil) {
        self.connection = connection
        self.role = role
        self.onRequest = onRequest
        self.onNotification = onNotification
        self.onClose = onClose
        nextID = role == .host ? 2 : 1
    }

    deinit { connection.close() }

    public func start() {
        let first: Bool = lock.withLock {
            defer { started = true }
            return !started
        }
        guard first else { return }
        var hello = Data([Wire.version, role.rawValue, 0, 0])
        hello.append(contentsOf: [0, 0, 0, 0])
        enqueue(Self.frame(.hello, id: 0, payload: hello))
        Task.detached { [self] in await self.readLoop() }
    }

    /// Disconnects; every pending operation fails with ECONNRESET.
    public func close() { fail(POSIXError(.ECONNRESET)) }

    public func request(_ opcode: UInt16, body: Data = Data(), end: Bool = false) async throws -> FileSessionStream {
        guard body.count <= Wire.frameMaximum - 4 else { throw POSIXError(.EMSGSIZE) }
        let id: UInt32 = try lock.withLock {
            if let failure { throw failure }
            guard nextID <= UInt32.max - 2 else { throw POSIXError(.EOVERFLOW) }
            // IDs must reach the wire in increasing order: allocate while queueing.
            let id = nextID; nextID += 2
            let state = StreamState(id: id, responder: false)
            state.localEnded = end
            streams[id] = state
            frames.append(Frame(bytes: Self.frame(.request, flags: end ? Wire.endStream : 0, id: id,
                                                   payload: Self.opcode(opcode) + body), done: nil))
            return id
        }
        pump()
        return FileSessionStream(session: self, id: id)
    }

    public func notify(_ opcode: UInt16, body: Data = Data()) async throws {
        guard body.count <= Wire.frameMaximum - 4 else { throw POSIXError(.EMSGSIZE) }
        try await write(Self.frame(.notify, id: 0, payload: Self.opcode(opcode) + body))
    }

    // MARK: Stream operations

    func respond(_ id: UInt32, status: UInt32, body: Data, end: Bool) async throws {
        guard body.count <= Wire.frameMaximum - 4 else { throw POSIXError(.EMSGSIZE) }
        var payload = Self.encode(status); payload.append(body)
        try await write(Self.frame(.response, flags: end ? Wire.endStream : 0, id: id, payload: payload)) { state in
            guard state.responder, !state.responded else { throw POSIXError(.EINVAL) }
            state.responded = true
            if end { state.localEnded = true }
        }
    }

    func send(_ id: UInt32, _ bytes: Data, end: Bool) async throws {
        if bytes.isEmpty && !end { return }
        // Refuse the whole send rather than end it part way.
        try lock.withLock {
            guard let state = streams[id] else { throw POSIXError(.ECANCELED) }
            guard bytes.count <= Wire.streamDataMaximum - state.sent else { throw POSIXError(.EMSGSIZE) }
        }
        var offset = bytes.startIndex
        repeat {
            let size = min(bytes.endIndex - offset, Wire.frameMaximum)
            let last = end && offset + size == bytes.endIndex
            let frame = Self.frame(.data, flags: last ? Wire.endStream : 0, id: id,
                                   payload: bytes.subdata(in: offset..<offset + size))
            try await write(frame) { state in
                if state.responder && !state.responded { throw POSIXError(.EINVAL) }
                guard size <= Wire.streamDataMaximum - state.sent else { throw POSIXError(.EMSGSIZE) }
                state.sent += size
                if last { state.localEnded = true }
            }
            offset += size
        } while offset < bytes.endIndex
    }

    func receive(_ id: UInt32) async throws -> Message? {
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                lock.lock(); defer { lock.unlock() }
                guard let state = streams[id] else { continuation.resume(throwing: POSIXError(.ECANCELED)); return }
                guard state.receiver == nil else { continuation.resume(throwing: POSIXError(.EBUSY)); return }
                if !state.inbound.isEmpty { continuation.resume(returning: takeLocked(state)) }
                else if state.cancelled { continuation.resume(throwing: POSIXError(.ECANCELED)) }
                else if state.remoteEnded { continuation.resume(returning: nil) }
                else if let failure { continuation.resume(throwing: failure) }
                else { state.receiver = continuation }
            }
        } onCancel: { self.cancel(id) }
    }

    func cancel(_ id: UInt32) {
        let send: Bool = lock.withLock {
            guard let state = streams[id], !state.cancelled else { return false }
            state.cancelled = true
            discardLocked(state)
            deliverLocked(state)
            return failure == nil && !state.finished
        }
        if send { enqueue(Self.frame(.cancel, id: id, payload: Data())) }
    }

    /// Either side cancelled the stream, or the session ended.
    func isCancelled(_ id: UInt32) -> Bool {
        lock.withLock { failure != nil || streams[id]?.cancelled ?? true }
    }

    /// The last stream reference is gone: cancel unless finished, then forget.
    func release(_ id: UInt32) {
        cancel(id)
        lock.withLock {
            guard let state = streams.removeValue(forKey: id) else { return }
            discardLocked(state)
            if state.responder { inboundStreams -= 1 }
        }
        pump()
    }

    // MARK: Writing

    private func write(_ bytes: Data, update: ((StreamState) throws -> Void)? = nil) async throws {
        let id = bytes.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: 8, as: UInt32.self)) }
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            let queued: Bool = lock.withLock {
                if let failure { continuation.resume(throwing: failure); return false }
                if let update {
                    guard let state = streams[id] else { continuation.resume(throwing: POSIXError(.ECANCELED)); return false }
                    do { try usableLocked(state); try update(state) }
                    catch { continuation.resume(throwing: error); return false }
                }
                frames.append(Frame(bytes: bytes, done: continuation))
                return true
            }
            // The continuation completes only once the frame is written.
            if queued { pump() }
        }
    }

    private func enqueue(_ bytes: Data) {
        lock.withLock {
            guard failure == nil else { return }
            frames.append(Frame(bytes: bytes, done: nil))
        }
        pump()
    }

    private func pump() {
        let next: Frame? = lock.withLock {
            guard !writing, failure == nil else { return nil }
            guard !frames.isEmpty else { return nil }
            writing = true
            return frames.removeFirst()
        }
        guard let next else { return }
        connection.send(next.bytes) { [self] result in
            lock.withLock { writing = false }
            switch result {
            case .success: next.done?.resume()
            case .failure(let error):
                next.done?.resume(throwing: POSIXError(.ECONNRESET))
                fail(error)
            }
            pump()
        }
    }

    /// Refusals the reader originates; a peer that never reads them is dropped.
    private func appendRefusalLocked(_ bytes: Data) throws {
        guard frames.count < Wire.maximumPendingControl else { throw POSIXError(.ENOBUFS) }
        frames.append(Frame(bytes: bytes, done: nil))
    }

    private func usableLocked(_ state: StreamState) throws {
        if state.cancelled { throw POSIXError(.ECANCELED) }
        if let failure { throw failure }
        if state.localEnded { throw POSIXError(.EPIPE) }
    }

    // MARK: Reading

    private func readLoop() async {
        do {
            let hello = try await readFrame()
            guard hello.type == .hello, hello.id == 0, hello.payload.count == 8,
                  hello.payload[0] == Wire.version, hello.payload[1] != role.rawValue,
                  hello.payload[1] <= Role.guest.rawValue, hello.payload[2] == 0, hello.payload[3] == 0
            else { throw POSIXError(.EPROTO) }
            while true {
                let frame = try await readFrame()
                let action = try lock.withLock { try dispatchLocked(frame) }
                action?()
                pump()
            }
        } catch {
            fail(Self.sessionError(error))
        }
    }

    private struct InboundFrame { let type: FrameType; let flags: UInt8; let id: UInt32; let payload: Data }

    private func readFrame() async throws -> InboundFrame {
        let header = try await connection.readExactly(Wire.headerSize)
        let id = header.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: 8, as: UInt32.self)) }
        let length = header.withUnsafeBytes { Int(UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: 12, as: UInt32.self))) }
        guard header.prefix(4) == Data("NP2F".utf8), header[header.startIndex + 6] == 0,
              header[header.startIndex + 7] == 0, length <= Wire.frameMaximum,
              let type = FrameType(rawValue: header[header.startIndex + 4]) else { throw POSIXError(.EPROTO) }
        let flags = header[header.startIndex + 5]
        let mayEnd = type == .request || type == .response || type == .data
        guard flags & ~(mayEnd ? Wire.endStream : 0) == 0 else { throw POSIXError(.EPROTO) }
        let payload = length == 0 ? Data() : try await connection.readExactly(length)
        return InboundFrame(type: type, flags: flags, id: id, payload: Data(payload))
    }

    private func isPeerID(_ id: UInt32) -> Bool {
        id != 0 && (id & 1) != (role == .guest ? 1 : 0)
    }

    /// Returns work to run after the lock is released (handler callbacks).
    private func dispatchLocked(_ frame: InboundFrame) throws -> (() -> Void)? {
        let id = frame.id, payload = frame.payload, end = frame.flags & Wire.endStream != 0
        let state = id == 0 ? nil : streams[id]
        switch frame.type {
        case .hello:
            throw POSIXError(.EPROTO)
        case .cancel:
            guard id != 0, payload.isEmpty else { throw POSIXError(.EPROTO) }
            if let state, !state.cancelled {
                state.cancelled = true
                discardLocked(state)
                deliverLocked(state)
            }
            return nil
        case .request:
            guard isPeerID(id), id > lastPeerID, payload.count >= 4 else { throw POSIXError(.EPROTO) }
            lastPeerID = id
            let opcode = Self.decode16(payload)
            guard let onRequest, inboundStreams < Wire.maximumInboundStreams else {
                try appendRefusalLocked(Self.frame(.cancel, id: id, payload: Data()))
                return nil
            }
            let state = StreamState(id: id, responder: true)
            state.remoteEnded = end
            streams[id] = state
            inboundStreams += 1
            let body = payload.dropFirst(4)
            return { [self] in onRequest(FileSessionStream(session: self, id: id), opcode, Data(body)) }
        case .response:
            guard id != 0, !isPeerID(id), payload.count >= 4 else { throw POSIXError(.EPROTO) }
            guard let state else {
                if id < nextID { return nil }
                throw POSIXError(.EPROTO)
            }
            if state.cancelled { return nil }
            guard !state.responded, !state.remoteEnded else { throw POSIXError(.EPROTO) }
            state.responded = true
            state.inbound.append(Message(kind: .response(status: Self.decode32(payload, at: 0)),
                                         body: Data(payload.dropFirst(4)), end: end))
            if end { state.remoteEnded = true }
            deliverLocked(state)
            return nil
        case .data:
            guard id != 0,
                  isPeerID(id) ? id <= lastPeerID : id < nextID else { throw POSIXError(.EPROTO) }
            // Abandoned stream: drop its data.
            guard let state, !state.cancelled else { return nil }
            guard !state.remoteEnded, state.responder || state.responded,
                  payload.count <= Wire.streamDataMaximum - state.received else { throw POSIXError(.EPROTO) }
            state.received += payload.count
            // An empty DATA only carries END; it is not delivered as a message.
            if !payload.isEmpty { state.inbound.append(Message(kind: .data, body: payload, end: end)) }
            if end { state.remoteEnded = true }
            deliverLocked(state)
            return nil
        case .notify:
            guard id == 0, payload.count >= 4 else { throw POSIXError(.EPROTO) }
            guard let onNotification else { return nil }
            let opcode = Self.decode16(payload), body = Data(payload.dropFirst(4))
            return { onNotification(opcode, body) }
        }
    }

    /// Pops one message for a waiting receiver, or completes it at end/cancel.
    private func deliverLocked(_ state: StreamState) {
        guard let receiver = state.receiver else { return }
        if !state.inbound.isEmpty {
            state.receiver = nil
            receiver.resume(returning: takeLocked(state))
        } else if state.cancelled || failure != nil {
            state.receiver = nil
            receiver.resume(throwing: state.cancelled ? POSIXError(.ECANCELED) : failure!)
        } else if state.remoteEnded {
            state.receiver = nil
            receiver.resume(returning: nil)
        }
    }

    private func takeLocked(_ state: StreamState) -> Message { state.inbound.removeFirst() }

    /// Unconsumed DATA will never be read.
    private func discardLocked(_ state: StreamState) { state.inbound.removeAll() }

    private func fail(_ error: Error) {
        let (first, frames, states): (Bool, [Frame], [StreamState]) = lock.withLock {
            guard failure == nil else { return (false, [], []) }
            failure = error
            let pending = frames
            frames = []
            return (true, pending, Array(streams.values))
        }
        guard first else { return }
        connection.close()
        frames.forEach { $0.done?.resume(throwing: error) }
        lock.withLock { states.forEach { deliverLocked($0) } }
        onClose?(error)
    }

    private static func sessionError(_ error: Error) -> Error {
        if let error = error as? POSIXError, [.EPROTO, .ENOBUFS].contains(error.code) { return error }
        return POSIXError(.ECONNRESET)
    }

    // MARK: Encoding

    static func frame(_ type: FrameType, flags: UInt8 = 0, id: UInt32, payload: Data) -> Data {
        var bytes = Data("NP2F".utf8)
        bytes.append(contentsOf: [type.rawValue, flags, 0, 0])
        bytes.append(encode(id)); bytes.append(encode(UInt32(payload.count)))
        bytes.append(payload)
        return bytes
    }
    static func opcode(_ value: UInt16) -> Data { Data([UInt8(value & 0xff), UInt8(value >> 8), 0, 0]) }
    static func encode(_ value: UInt32) -> Data { withUnsafeBytes(of: value.littleEndian) { Data($0) } }
    static func decode32(_ data: Data, at offset: Int) -> UInt32 {
        data.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self)) }
    }
    static func decode16(_ data: Data) -> UInt16 {
        data.withUnsafeBytes { UInt16(littleEndian: $0.loadUnaligned(as: UInt16.self)) }
    }
}

/// One NPFR v2 stream. Dropping the last reference before both directions
/// finish cancels it, matching `np_file2_stream_release`.
public final class FileSessionStream: @unchecked Sendable {
    public let id: UInt32
    private let session: FileSession

    init(session: FileSession, id: UInt32) { self.session = session; self.id = id }
    deinit { session.release(id) }

    /// The next RESPONSE or DATA message; nil once the peer's half has ended.
    /// Cancelling the calling task cancels the stream.
    public func receive() async throws -> FileSession.Message? { try await session.receive(id) }
    /// Splits into frames; throws EMSGSIZE beyond `Wire.streamDataMaximum` in
    /// total. Empty + end finishes.
    public func send(_ data: Data, end: Bool = false) async throws { try await session.send(id, data, end: end) }
    /// Responder only, exactly once, before any DATA.
    public func respond(status: UInt32 = 0, body: Data = Data(), end: Bool = false) async throws {
        try await session.respond(id, status: status, body: body, end: end)
    }
    public func cancel() { session.cancel(id) }
    /// For a responder doing long work: the requester gave up or disconnected.
    public var isCancelled: Bool { session.isCancelled(id) }
}
