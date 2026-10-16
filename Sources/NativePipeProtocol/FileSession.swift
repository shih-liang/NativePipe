import Foundation
import Darwin

/// NPFR v2: one long-lived, full-duplex connection carrying many concurrent
/// streams, matching `common/file_rpc/np_file_session.c`. Either side opens
/// streams and sends notifications; the role only decides stream ID parity.
///
/// Frames are written one at a time; control frames overtake queued DATA so
/// metadata requests stay responsive during large transfers. DATA is bounded
/// by per-stream and per-connection credit, so memory stays bounded and a
/// stalled reader blocks other streams only once stalled streams hold the
/// whole connection window (16 full stream windows).
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
        public static let streamWindow: UInt32 = 256 * 1024
        public static let connectionWindow: UInt32 = 4 * 1024 * 1024
        public static let maximumInboundStreams = 256
        /// Frames waiting while the peer is not reading; more drops the peer.
        public static let maximumPendingControl = 65536
        public static let port = NativePipePort.fileSession
        public static let endStream: UInt8 = 1
    }

    enum FrameType: UInt8 {
        case hello = 1, request, response, data, notify, cancel, credit
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
        var sendWindow = Wire.streamWindow, receiveWindow = Wire.streamWindow, consumed: UInt32 = 0
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
    private var sendWindow = Wire.connectionWindow
    private var receiveWindow = Wire.connectionWindow
    private var consumed: UInt32 = 0
    /// Senders waiting for credit; each retries its reservation when woken.
    private var creditWaiters: [() -> Void] = []
    private var control: [Frame] = []
    private var data: [Frame] = []
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
        enqueue(Self.frame(.hello, id: 0, payload: hello), priority: true)
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
            control.append(Frame(bytes: Self.frame(.request, flags: end ? Wire.endStream : 0, id: id,
                                                   payload: Self.opcode(opcode) + body), done: nil))
            return id
        }
        pump()
        return FileSessionStream(session: self, id: id)
    }

    public func notify(_ opcode: UInt16, body: Data = Data()) async throws {
        guard body.count <= Wire.frameMaximum - 4 else { throw POSIXError(.EMSGSIZE) }
        try await write(Self.frame(.notify, id: 0, payload: Self.opcode(opcode) + body), priority: true)
    }

    // MARK: Stream operations

    func respond(_ id: UInt32, status: UInt32, body: Data, end: Bool) async throws {
        guard body.count <= Wire.frameMaximum - 4 else { throw POSIXError(.EMSGSIZE) }
        var payload = Self.encode(status); payload.append(body)
        try await write(Self.frame(.response, flags: end ? Wire.endStream : 0, id: id, payload: payload),
                        priority: true) { state in
            guard state.responder, !state.responded else { throw POSIXError(.EINVAL) }
            state.responded = true
            if end { state.localEnded = true }
        }
    }

    func send(_ id: UInt32, _ bytes: Data, end: Bool) async throws {
        if bytes.isEmpty && !end { return }
        var offset = bytes.startIndex
        repeat {
            let size = try await reserveCredit(id, wanted: bytes.endIndex - offset)
            let last = end && offset + size == bytes.endIndex
            let frame = Self.frame(.data, flags: last ? Wire.endStream : 0, id: id,
                                   payload: bytes.subdata(in: offset..<offset + size))
            do {
                try await write(frame, priority: false) { state in
                    if last { state.localEnded = true }
                }
            } catch {
                refundCredit(id, size)
                throw error
            }
            offset += size
        } while offset < bytes.endIndex
    }

    func receive(_ id: UInt32) async throws -> Message? {
        defer { pump() } // consuming may have queued credit
        return try await withTaskCancellationHandler {
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
            wakeLocked(state)
            return failure == nil && !state.finished
        }
        if send { enqueue(Self.frame(.cancel, id: id, payload: Data()), priority: true) }
        pump()
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

    private func write(_ bytes: Data, priority: Bool,
                       update: ((StreamState) throws -> Void)? = nil) async throws {
        let id = bytes.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: 8, as: UInt32.self)) }
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            let queued: Bool = lock.withLock {
                if let failure { continuation.resume(throwing: failure); return false }
                if let update {
                    guard let state = streams[id] else { continuation.resume(throwing: POSIXError(.ECANCELED)); return false }
                    do { try usableLocked(state); try update(state) }
                    catch { continuation.resume(throwing: error); return false }
                }
                let frame = Frame(bytes: bytes, done: continuation)
                if priority { control.append(frame) } else { data.append(frame) }
                return true
            }
            // The continuation completes only once the frame is written.
            if queued { pump() }
        }
    }

    private func enqueue(_ bytes: Data, priority: Bool) {
        lock.withLock {
            guard failure == nil else { return }
            let frame = Frame(bytes: bytes, done: nil)
            if priority { control.append(frame) } else { data.append(frame) }
        }
        pump()
    }

    private func pump() {
        let next: Frame? = lock.withLock {
            guard !writing, failure == nil else { return nil }
            let frame = !control.isEmpty ? control.removeFirst() : !data.isEmpty ? data.removeFirst() : nil
            if frame != nil { writing = true }
            return frame
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

    private func reserveCredit(_ id: UInt32, wanted: Int) async throws -> Int {
        while true {
            let granted: Int? = try await withCheckedThrowingContinuation { continuation in
                lock.lock(); defer { lock.unlock() }
                do {
                    guard let state = streams[id] else { throw POSIXError(.ECANCELED) }
                    try usableLocked(state)
                    if state.responder && !state.responded { throw POSIXError(.EINVAL) }
                    if wanted == 0 { continuation.resume(returning: 0); return }
                    let size = min(wanted, Wire.frameMaximum, Int(state.sendWindow), Int(sendWindow))
                    if size > 0 {
                        state.sendWindow -= UInt32(size); sendWindow -= UInt32(size)
                        continuation.resume(returning: size); return
                    }
                    creditWaiters.append { continuation.resume(returning: nil) }
                } catch { continuation.resume(throwing: error) }
            }
            if let granted { return granted }
        }
    }

    /// Cancelled before queueing: the peer never sees these bytes, so the credit
    /// must be restored or the connection window shrinks for good.
    private func refundCredit(_ id: UInt32, _ size: Int) {
        lock.withLock {
            guard size > 0, failure == nil else { return }
            sendWindow += UInt32(size)
            streams[id]?.sendWindow += UInt32(size)
            wakeCreditLocked()
        }
    }

    /// Frames the reader originates; a peer that never reads them is dropped.
    private func appendPeerControlLocked(_ bytes: Data) throws {
        guard control.count < Wire.maximumPendingControl else { throw POSIXError(.ENOBUFS) }
        control.append(Frame(bytes: bytes, done: nil))
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
        case .credit:
            guard payload.count == 4 else { throw POSIXError(.EPROTO) }
            let increment = Self.decode32(payload, at: 0)
            guard increment > 0 else { throw POSIXError(.EPROTO) }
            if id == 0 {
                guard sendWindow <= UInt32.max / 2 - increment else { throw POSIXError(.EPROTO) }
                sendWindow += increment
            } else if let state, !state.localEnded {
                guard state.sendWindow <= UInt32.max / 2 - increment else { throw POSIXError(.EPROTO) }
                state.sendWindow += increment
            }
            wakeCreditLocked()
            return nil
        case .cancel:
            guard id != 0, payload.isEmpty else { throw POSIXError(.EPROTO) }
            if let state, !state.cancelled {
                state.cancelled = true
                discardLocked(state)
                wakeLocked(state)
            }
            return nil
        case .request:
            guard isPeerID(id), id > lastPeerID, payload.count >= 4 else { throw POSIXError(.EPROTO) }
            lastPeerID = id
            let opcode = Self.decode16(payload)
            guard let onRequest, inboundStreams < Wire.maximumInboundStreams else {
                try appendPeerControlLocked(Self.frame(.cancel, id: id, payload: Data()))
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
            let length = UInt32(payload.count)
            guard length <= receiveWindow else { throw POSIXError(.EPROTO) }
            receiveWindow -= length
            guard let state, !state.cancelled else {
                // Abandoned stream: return the connection credit immediately.
                receiveWindow += length
                if length > 0 { try appendPeerControlLocked(Self.credit(id: 0, length)) }
                return nil
            }
            guard !state.remoteEnded, state.responder || state.responded,
                  length <= state.receiveWindow else { throw POSIXError(.EPROTO) }
            state.receiveWindow -= length
            // An empty DATA only carries END; it is not delivered as a message.
            if length > 0 { state.inbound.append(Message(kind: .data, body: payload, end: end)) }
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

    /// Removes the next message and returns consumed credit to the peer.
    private func takeLocked(_ state: StreamState) -> Message {
        let message = state.inbound.removeFirst()
        guard message.kind == .data else { return message }
        let length = UInt32(message.body.count)
        state.consumed += length; consumed += length
        if consumed >= Wire.connectionWindow / 4 {
            receiveWindow += consumed
            control.append(Frame(bytes: Self.credit(id: 0, consumed), done: nil))
            consumed = 0
        }
        if !state.remoteEnded && state.consumed >= Wire.streamWindow / 4 {
            state.receiveWindow += state.consumed
            control.append(Frame(bytes: Self.credit(id: state.id, state.consumed), done: nil))
            state.consumed = 0
        }
        return message
    }

    /// Unconsumed DATA will never be read: return its connection credit.
    private func discardLocked(_ state: StreamState) {
        let discarded = state.inbound.reduce(UInt32(0)) {
            $1.kind == .data ? $0 + UInt32($1.body.count) : $0
        }
        state.inbound.removeAll()
        guard discarded > 0, failure == nil else { return }
        receiveWindow += discarded
        control.append(Frame(bytes: Self.credit(id: 0, discarded), done: nil))
    }

    private func wakeLocked(_ state: StreamState) {
        deliverLocked(state)
        wakeCreditLocked()
    }

    private func wakeCreditLocked() {
        let waiters = creditWaiters; creditWaiters = []
        waiters.forEach { $0() }
    }

    private func fail(_ error: Error) {
        let (first, frames, states): (Bool, [Frame], [StreamState]) = lock.withLock {
            guard failure == nil else { return (false, [], []) }
            failure = error
            let frames = control + data
            control = []; data = []
            return (true, frames, Array(streams.values))
        }
        guard first else { return }
        connection.close()
        frames.forEach { $0.done?.resume(throwing: error) }
        lock.withLock {
            states.forEach { deliverLocked($0) }
            wakeCreditLocked()
        }
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
    private static func credit(id: UInt32, _ increment: UInt32) -> Data {
        frame(.credit, id: id, payload: encode(increment))
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
    /// Waits for flow-control credit; splits into frames. Empty + end finishes.
    public func send(_ data: Data, end: Bool = false) async throws { try await session.send(id, data, end: end) }
    /// Responder only, exactly once, before any DATA.
    public func respond(status: UInt32 = 0, body: Data = Data(), end: Bool = false) async throws {
        try await session.respond(id, status: status, body: body, end: end)
    }
    public func cancel() { session.cancel(id) }
    /// For a responder doing long work: the requester gave up or disconnected.
    public var isCancelled: Bool { session.isCancelled(id) }
}
