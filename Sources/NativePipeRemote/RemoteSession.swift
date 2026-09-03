import Foundation
import NativePipeProtocol
#if canImport(Darwin)
import Darwin
#endif

enum RemoteSessionProtocolError: Error, Equatable {
    case expectedChannelReady
    case duplicateChannelReady
}

enum RemoteLaneHandshake {
    enum Lane: UInt8 {
        case surface = 1
        case media = 2
    }

    static let byteCount = 16

    static func frame(token: UInt64, lane: Lane) -> Data {
        precondition(token != 0)
        var data = Data("NPRH".utf8)
        data.append(1)
        data.append(lane.rawValue)
        data.append(contentsOf: [0, 0])
        var littleEndianToken = token.littleEndian
        withUnsafeBytes(of: &littleEndianToken) { data.append(contentsOf: $0) }
        precondition(data.count == byteCount)
        return data
    }
}

/// Cancellation/generation probe passed to the blocking connection opener.
/// Keeping this as a value makes the production resolver and deterministic
/// socketpair tests share the exact same lifecycle path.
struct RemoteConnectionAttempt: Sendable {
    private let probe: @Sendable () -> Bool

    init(_ probe: @escaping @Sendable () -> Bool) {
        self.probe = probe
    }

    var shouldContinue: Bool { probe() }
}

typealias RemoteConnectionOpener = @Sendable (
    _ host: String, _ port: UInt16, _ attempt: RemoteConnectionAttempt
) throws -> Int32

enum RemoteMediaReadinessError: Error, Equatable {
    case bufferLimitExceeded
}

/// Holds frames that belong to the current nonce but outran channelReady on
/// the independent media TCP stream.  They are released only after the surface
/// stream has delivered its authoritative session snapshot gate.
struct RemoteMediaReadinessBuffer {
    typealias Frame = (MediaWire.Header, Data)

    private(set) var byteCount = 0
    private var frames: [Frame] = []
    let byteLimit: Int

    init(byteLimit: Int = 2 * MediaWire.maximumPayloadSize) {
        self.byteLimit = byteLimit
    }

    mutating func hold(_ newFrames: [Frame]) throws {
        let added = newFrames.reduce(0) { partial, frame in
            partial + MediaWire.headerSize + frame.1.count
        }
        guard added <= byteLimit - byteCount else {
            throw RemoteMediaReadinessError.bufferLimitExceeded
        }
        frames.append(contentsOf: newFrames)
        byteCount += added
    }

    mutating func releaseAll() -> [Frame] {
        defer {
            frames.removeAll(keepingCapacity: true)
            byteCount = 0
        }
        return frames
    }

    mutating func reset() {
        frames.removeAll(keepingCapacity: true)
        byteCount = 0
    }
}

struct RemoteSurfaceHandshake {
    private(set) var isReady = false

    /// Returns true exactly once, when the transport becomes usable.
    mutating func accept(_ event: Windowing.GuestEvent) throws -> Bool {
        if !isReady {
            guard case .channelReady = event else {
                throw RemoteSessionProtocolError.expectedChannelReady
            }
            isReady = true
            return true
        }
        if case .channelReady = event {
            throw RemoteSessionProtocolError.duplicateChannelReady
        }
        return false
    }
}

/// TCP client for NativePipe window (NPIP) and media (NPEN) ports.
///
/// Assumes the remote compositor is listening on loopback and reached via
/// `ssh -L` (or a direct LAN connection). Does not embed SSH itself.
public final class RemoteSession: @unchecked Sendable {
    public enum State: Sendable, Equatable {
        case disconnected
        case connected
    }

    private let host: String
    private let surfacePort: UInt16
    private let mediaPort: UInt16
    private let connectionOpener: RemoteConnectionOpener

    /// Serializes the public lifecycle around socket creation and publication.
    /// `lock` protects already-published state, but cannot by itself cover the
    /// blocking gap between opening two lanes and installing their generation.
    private let lifecycleLock = NSLock()
    private let lock = NSLock()
    private var surfaceFD: Int32 = -1
    private var mediaFD: Int32 = -1
    private var surfaceSource: DispatchSourceRead?
    private var mediaSource: DispatchSourceRead?
    private var surfaceDecoder = FrameDecoder()
    private var surfaceHandshake = RemoteSurfaceHandshake()
    private var mediaDemuxer = MediaWire.Demuxer()
    private var earlyMedia = RemoteMediaReadinessBuffer()
    private var generation: UInt64 = 0
    private let connectQueue = DispatchQueue(
        label: "com.nativepipe.remote.connect",
        qos: .userInitiated,
        attributes: .concurrent)
    private let readQueue = DispatchQueue(label: "com.nativepipe.remote.read", qos: .userInteractive)
    private let writeQueue = DispatchQueue(label: "com.nativepipe.remote.write", qos: .userInteractive)
    private var pendingWrites: [Windowing.HostCommand] = []
    private var writerScheduled = false
    private var eventHandler: ((Windowing.GuestEvent) -> Void)?
    private var mediaFrameHandler: ((MediaWire.Header, Data) -> Void)?
    private var stateChangeHandler: ((State) -> Void)?

    /// Callback storage shares the session lock with transport state. The
    /// callbacks themselves are always invoked after releasing it, so callers
    /// may safely replace a handler while a background read completes.
    public var onEvent: ((Windowing.GuestEvent) -> Void)? {
        get {
            lock.lock(); defer { lock.unlock() }
            return eventHandler
        }
        set {
            lock.lock(); defer { lock.unlock() }
            eventHandler = newValue
        }
    }

    public var onMediaFrame: ((MediaWire.Header, Data) -> Void)? {
        get {
            lock.lock(); defer { lock.unlock() }
            return mediaFrameHandler
        }
        set {
            lock.lock(); defer { lock.unlock() }
            mediaFrameHandler = newValue
        }
    }

    public var onStateChange: ((State) -> Void)? {
        get {
            lock.lock(); defer { lock.unlock() }
            return stateChangeHandler
        }
        set {
            lock.lock(); defer { lock.unlock() }
            stateChangeHandler = newValue
        }
    }

    public convenience init(
        host: String = "127.0.0.1",
        surfacePort: UInt16 = UInt16(NativePipePort.surface),
        mediaPort: UInt16 = UInt16(NativePipePort.media)
    ) {
        self.init(
            host: host,
            surfacePort: surfacePort,
            mediaPort: mediaPort,
            connectionOpener: { host, port, attempt in
                try Self.openTCP(host: host, port: port, attempt: attempt)
            })
    }

    init(
        host: String,
        surfacePort: UInt16,
        mediaPort: UInt16,
        connectionOpener: @escaping RemoteConnectionOpener
    ) {
        self.host = host
        self.surfacePort = surfacePort
        self.mediaPort = mediaPort
        self.connectionOpener = connectionOpener
    }

    deinit {
        // Dispatch sources do not own their file descriptors. Every queued
        // callback that entered a method already retains `self`, so deinit can
        // only run after those uses finish; close synchronously here instead of
        // scheduling a block that would retain an object being destroyed.
        surfaceSource?.cancel()
        mediaSource?.cancel()
        if surfaceFD >= 0 { Darwin.close(surfaceFD) }
        if mediaFD >= 0 { Darwin.close(mediaFD) }
    }

    public var isConnected: Bool {
        lock.lock(); defer { lock.unlock() }
        return surfaceFD >= 0 && mediaFD >= 0 && surfaceHandshake.isReady
    }

    /// Connect both channels without occupying the caller's executor. A newer
    /// connect or disconnect invalidates this attempt and its nonblocking polls
    /// notice within 50 ms.
    public func connect() async throws {
        try Task.checkCancellation()
        let connectionGeneration = beginConnectionAttempt()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation {
                (continuation: CheckedContinuation<Void, Error>) in
                connectQueue.async { [weak self] in
                    guard let self else {
                        continuation.resume(throwing: CancellationError())
                        return
                    }
                    do {
                        try self.performConnection(
                            expectedGeneration: connectionGeneration)
                        continuation.resume()
                    } catch {
                        continuation.resume(throwing: error)
                    }
                }
            }
        } onCancel: {
            self.disconnect(expectedGeneration: connectionGeneration)
        }
        try Task.checkCancellation()
    }

    /// DNS and the two nonblocking connect/poll loops are deliberately run on
    /// `connectQueue`; callers normally invoke `connect()` from MainActor.
    private func performConnection(expectedGeneration connectionGeneration: UInt64) throws {
        let attempt = RemoteConnectionAttempt { [weak self] in
            self?.isCurrentAttempt(connectionGeneration) == true
        }
        let surface = try connectionOpener(host, surfacePort, attempt)
        let media: Int32
        do {
            media = try connectionOpener(host, mediaPort, attempt)
        } catch {
            Darwin.close(surface)
            throw error
        }
        var token = UInt64(arc4random()) << 32 | UInt64(arc4random())
        if token == 0 { token = 1 }
        try installConnectedLanes(
            surface: surface,
            media: media,
            token: token,
            expectedGeneration: connectionGeneration)
    }

    /// Keeps the blocking lock API out of the async function body. The SDK
    /// marks `NSLock.lock()` unavailable directly from async contexts because
    /// suspending while holding it would deadlock an executor; these helpers do
    /// not suspend.
    private func beginConnectionAttempt() -> UInt64 {
        lifecycleLock.lock()
        defer { lifecycleLock.unlock() }
        disconnectLocked(expectedGeneration: nil)
        lock.lock()
        defer { lock.unlock() }
        return generation
    }

    private func installConnectedLanes(
        surface: Int32,
        media: Int32,
        token: UInt64,
        expectedGeneration: UInt64
    ) throws {
        lifecycleLock.lock()
        defer { lifecycleLock.unlock() }
        guard isCurrentAttempt(expectedGeneration) else {
            Darwin.close(surface)
            Darwin.close(media)
            throw CancellationError()
        }
        do {
            try Self.writeAll(
                fd: surface,
                data: RemoteLaneHandshake.frame(token: token, lane: .surface))
            try Self.writeAll(
                fd: media,
                data: RemoteLaneHandshake.frame(token: token, lane: .media))
        } catch {
            Darwin.close(surface)
            Darwin.close(media)
            throw error
        }
        lock.lock()
        guard generation == expectedGeneration else {
            lock.unlock()
            Darwin.close(surface)
            Darwin.close(media)
            throw CancellationError()
        }
        surfaceFD = surface
        mediaFD = media
        surfaceDecoder = FrameDecoder()
        surfaceHandshake = RemoteSurfaceHandshake()
        mediaDemuxer = MediaWire.Demuxer()
        earlyMedia.reset()
        pendingWrites.removeAll(keepingCapacity: true)
        writerScheduled = false
        lock.unlock()
        armSurfaceReader(surface, generation: expectedGeneration)
        armMediaReader(media, generation: expectedGeneration)
    }

    private func isCurrentAttempt(_ expectedGeneration: UInt64) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return generation == expectedGeneration && surfaceFD < 0 && mediaFD < 0
    }

    public func disconnect() {
        disconnect(expectedGeneration: nil)
    }

    private func disconnect(expectedGeneration: UInt64?) {
        lifecycleLock.lock()
        defer { lifecycleLock.unlock() }
        disconnectLocked(expectedGeneration: expectedGeneration)
    }

    /// Called only while `lifecycleLock` is held.
    private func disconnectLocked(expectedGeneration: UInt64?) {
        lock.lock()
        if let expectedGeneration, generation != expectedGeneration {
            lock.unlock()
            return
        }
        let surface = surfaceFD
        let media = mediaFD
        let surfaceSource = self.surfaceSource
        let mediaSource = self.mediaSource
        let hadConnection = surface >= 0 || media >= 0
        generation &+= 1
        surfaceFD = -1
        mediaFD = -1
        surfaceHandshake = RemoteSurfaceHandshake()
        earlyMedia.reset()
        self.surfaceSource = nil
        self.mediaSource = nil
        pendingWrites.removeAll(keepingCapacity: true)
        lock.unlock()
        // Wake any read/write already using this socket without closing its
        // descriptor number out from under that callback. The deferred close
        // below still prevents descriptor reuse until both serial queues have
        // crossed the old generation boundary.
        if surface >= 0 { _ = Darwin.shutdown(surface, SHUT_RDWR) }
        if media >= 0 { _ = Darwin.shutdown(media, SHUT_RDWR) }
        surfaceSource?.cancel()
        mediaSource?.cancel()
        // Do not close a descriptor while an older read/write callback may
        // still be using it: Darwin can immediately recycle that integer for
        // the replacement connection. Closing after both serial queues reach
        // this generation boundary makes descriptor identity reliable.
        if surface >= 0 {
            readQueue.async {
                self.writeQueue.async { Darwin.close(surface) }
            }
        }
        if media >= 0 {
            readQueue.async { Darwin.close(media) }
        }
        if hadConnection {
            let disconnectedGeneration = generation
            DispatchQueue.main.async {
                self.lock.lock()
                let isStillDisconnected = self.generation == disconnectedGeneration
                    && self.surfaceFD < 0 && self.mediaFD < 0
                self.lock.unlock()
                if isStillDisconnected { self.onStateChange?(.disconnected) }
            }
        }
    }

    public func send(_ command: Windowing.HostCommand) {
        lock.lock()
        guard surfaceFD >= 0, surfaceHandshake.isReady else {
            lock.unlock()
            return
        }
        var replaced = false
        if let previous = pendingWrites.last {
            switch (command, previous) {
            case (.pointerMoved(let window, _, _),
                  .pointerMoved(let previousWindow, _, _)) where window == previousWindow:
                pendingWrites[pendingWrites.count - 1] = command
                replaced = true
            case (.configure(let window, _, _, _),
                  .configure(let previousWindow, _, _, _)) where window == previousWindow:
                pendingWrites[pendingWrites.count - 1] = command
                replaced = true
            case (.pointerScroll(let window, let dx, let dy, let precise),
                  .pointerScroll(let previousWindow, let previousDX, let previousDY,
                                 let previousPrecise))
                where window == previousWindow && precise == previousPrecise:
                pendingWrites[pendingWrites.count - 1] = .pointerScroll(
                    window: window, dx: previousDX + dx, dy: previousDY + dy,
                    isPrecise: precise)
                replaced = true
            default:
                break
            }
        }
        if !replaced { pendingWrites.append(command) }
        let shouldSchedule = !writerScheduled
        writerScheduled = true
        lock.unlock()
        if shouldSchedule {
            writeQueue.async { self.drainWrites() }
        }
    }

    private func drainWrites() {
        while true {
            lock.lock()
            guard !pendingWrites.isEmpty, surfaceFD >= 0, surfaceHandshake.isReady else {
                writerScheduled = false
                lock.unlock()
                return
            }
            let command = pendingWrites.removeFirst()
            let fd = surfaceFD
            let connectionGeneration = generation
            lock.unlock()

            do {
                let payload = try WindowWire.commandPayload(for: command)
                let framed = try WireFormat.frame(payload: payload)
                lock.lock()
                let isCurrent = generation == connectionGeneration && surfaceFD == fd
                lock.unlock()
                guard isCurrent else { continue }
                try Self.writeAll(fd: fd, data: framed)
            } catch {
                disconnect(expectedGeneration: connectionGeneration)
                return
            }
        }
    }

    private func armSurfaceReader(_ fd: Int32, generation: UInt64) {
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: readQueue)
        source.setEventHandler { [weak self] in
            self?.drainSurface(fd: fd, generation: generation)
        }
        lock.lock()
        surfaceSource?.cancel()
        surfaceSource = source
        lock.unlock()
        source.resume()
        readQueue.async { [weak self] in
            self?.drainSurface(fd: fd, generation: generation)
        }
    }

    private func armMediaReader(_ fd: Int32, generation: UInt64) {
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: readQueue)
        source.setEventHandler { [weak self] in
            self?.drainMedia(fd: fd, generation: generation)
        }
        lock.lock()
        mediaSource?.cancel()
        mediaSource = source
        lock.unlock()
        source.resume()
        readQueue.async { [weak self] in
            self?.drainMedia(fd: fd, generation: generation)
        }
    }

    private func drainSurface(fd: Int32, generation: UInt64) {
        lock.lock()
        let isCurrent = self.generation == generation && surfaceFD == fd
        lock.unlock()
        guard isCurrent else { return }

        var buffer = [UInt8](repeating: 0, count: 256 * 1024)
        let n = Darwin.recv(fd, &buffer, buffer.count, 0)
        if n == 0 {
            fputs("nativepipe-remote: surface channel EOF\n", stderr)
            fflush(stderr)
            disconnect(expectedGeneration: generation)
            return
        }
        if n < 0 {
            if errno == EAGAIN || errno == EWOULDBLOCK { return }
            fputs("nativepipe-remote: surface read errno=\(errno)\n", stderr)
            fflush(stderr)
            disconnect(expectedGeneration: generation)
            return
        }
        let data = Data(buffer.prefix(n))
        lock.lock()
        guard self.generation == generation, surfaceFD == fd else {
            lock.unlock()
            return
        }
        surfaceDecoder.append(data)
        var events: [Windowing.GuestEvent] = []
        var becameReady = false
        var releasedMedia: [RemoteMediaReadinessBuffer.Frame] = []
        do {
            while let payload = try surfaceDecoder.next() {
                let event = try WindowWire.guestEvent(from: payload)
                becameReady = try surfaceHandshake.accept(event) || becameReady
                events.append(event)
            }
            if becameReady { releasedMedia = earlyMedia.releaseAll() }
        } catch {
            lock.unlock()
            fputs("nativepipe-remote: surface protocol error: \(error)\n", stderr)
            fflush(stderr)
            disconnect(expectedGeneration: generation)
            return
        }
        lock.unlock()
        guard !events.isEmpty else { return }
        let readyToDeliver = becameReady
        let eventsToDeliver = events
        let mediaToDeliver = releasedMedia
        DispatchQueue.main.async {
            self.lock.lock()
            let isCurrent = self.generation == generation
                && self.surfaceFD == fd && self.surfaceHandshake.isReady
            self.lock.unlock()
            guard isCurrent else { return }
            if readyToDeliver { self.onStateChange?(.connected) }
            for event in eventsToDeliver { self.onEvent?(event) }
            for (header, payload) in mediaToDeliver {
                self.onMediaFrame?(header, payload)
            }
        }
    }

    private func drainMedia(fd: Int32, generation: UInt64) {
        lock.lock()
        let isCurrent = self.generation == generation && mediaFD == fd
        lock.unlock()
        guard isCurrent else { return }

        var buffer = [UInt8](repeating: 0, count: 256 * 1024)
        let n = Darwin.recv(fd, &buffer, buffer.count, 0)
        if n == 0 {
            fputs("nativepipe-remote: media channel EOF\n", stderr)
            fflush(stderr)
            disconnect(expectedGeneration: generation)
            return
        }
        if n < 0 {
            if errno == EAGAIN || errno == EWOULDBLOCK { return }
            fputs("nativepipe-remote: media read errno=\(errno)\n", stderr)
            fflush(stderr)
            disconnect(expectedGeneration: generation)
            return
        }
        lock.lock()
        guard self.generation == generation, mediaFD == fd else {
            lock.unlock()
            return
        }
        let frames = mediaDemuxer.push(Data(buffer.prefix(n)))
        let surfaceIsReady = surfaceHandshake.isReady
        if !surfaceIsReady && !frames.isEmpty {
            do {
                try earlyMedia.hold(frames)
            } catch {
                lock.unlock()
                fputs("nativepipe-remote: media arrived before channelReady beyond buffer limit\n", stderr)
                fflush(stderr)
                disconnect(expectedGeneration: generation)
                return
            }
        }
        lock.unlock()
        guard surfaceIsReady, !frames.isEmpty else { return }
        // Deliver on the same main-queue boundary as lifecycle events. Because
        // readQueue is serial, a channelReady block already enqueued by the
        // surface drain stays ahead of this one.
        DispatchQueue.main.async {
            self.lock.lock()
            let isCurrent = self.generation == generation
                && self.mediaFD == fd && self.surfaceHandshake.isReady
            self.lock.unlock()
            guard isCurrent else { return }
            for (header, payload) in frames {
                self.onMediaFrame?(header, payload)
            }
        }
    }

    private static func openTCP(
        host: String, port: UInt16, attempt: RemoteConnectionAttempt
    ) throws -> Int32 {
        var hints = addrinfo()
        hints.ai_flags = AI_ADDRCONFIG
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM
        hints.ai_protocol = IPPROTO_TCP
        var addresses: UnsafeMutablePointer<addrinfo>?
        let status = String(port).withCString { service in
            host.withCString { name in
                getaddrinfo(name, service, &hints, &addresses)
            }
        }
        guard status == 0, let first = addresses else {
            throw POSIXError(.EHOSTUNREACH)
        }
        defer { freeaddrinfo(first) }

        let deadline = DispatchTime.now().uptimeNanoseconds + 10_000_000_000
        var candidate: UnsafeMutablePointer<addrinfo>? = first
        var lastError: POSIXErrorCode = .ECONNREFUSED
        while let current = candidate {
            let address = current.pointee
            candidate = address.ai_next
            guard attempt.shouldContinue else {
                throw CancellationError()
            }
            let fd = Darwin.socket(
                address.ai_family, address.ai_socktype, address.ai_protocol)
            if fd < 0 {
                lastError = POSIXErrorCode(rawValue: errno) ?? .EIO
                continue
            }
            let descriptorFlags = fcntl(fd, F_GETFD)
            if descriptorFlags >= 0 {
                _ = fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC)
            }
            let statusFlags = fcntl(fd, F_GETFL)
            if statusFlags < 0 || fcntl(fd, F_SETFL, statusFlags | O_NONBLOCK) != 0 {
                lastError = POSIXErrorCode(rawValue: errno) ?? .EIO
                Darwin.close(fd)
                continue
            }

            let connected = Darwin.connect(fd, address.ai_addr, address.ai_addrlen)
            if connected == 0 { return fd }
            if errno != EINPROGRESS {
                lastError = POSIXErrorCode(rawValue: errno) ?? .ECONNREFUSED
                Darwin.close(fd)
                continue
            }
            while DispatchTime.now().uptimeNanoseconds < deadline {
                guard attempt.shouldContinue else {
                    Darwin.close(fd)
                    throw CancellationError()
                }
                var descriptor = pollfd(fd: fd, events: Int16(POLLOUT), revents: 0)
                let ready = Darwin.poll(&descriptor, 1, 50)
                if ready == 0 { continue }
                if ready < 0 {
                    if errno == EINTR { continue }
                    lastError = POSIXErrorCode(rawValue: errno) ?? .EIO
                    break
                }
                var socketError: Int32 = 0
                var socketErrorSize = socklen_t(MemoryLayout<Int32>.size)
                if getsockopt(
                    fd, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorSize) == 0,
                   socketError == 0 {
                    return fd
                }
                lastError = POSIXErrorCode(rawValue: socketError) ?? .ECONNREFUSED
                break
            }
            Darwin.close(fd)
            if DispatchTime.now().uptimeNanoseconds >= deadline {
                throw POSIXError(.ETIMEDOUT)
            }
        }
        throw POSIXError(lastError)
    }

    private static func writeAll(fd: Int32, data: Data) throws {
        try data.withUnsafeBytes { raw in
            var sent = 0
            let total = raw.count
            let deadline = DispatchTime.now().uptimeNanoseconds + 10_000_000_000
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            while sent < total {
                guard DispatchTime.now().uptimeNanoseconds < deadline else {
                    throw POSIXError(.ETIMEDOUT)
                }
                let n = Darwin.send(fd, base.advanced(by: sent), total - sent, MSG_NOSIGNAL)
                if n > 0 {
                    sent += n
                    continue
                }
                if n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) {
                    var descriptor = pollfd(fd: fd, events: Int16(POLLOUT), revents: 0)
                    let ready = Darwin.poll(&descriptor, 1, 250)
                    if ready > 0,
                       descriptor.revents & Int16(POLLERR | POLLHUP | POLLNVAL) == 0 {
                        continue
                    }
                    if ready < 0 && errno == EINTR { continue }
                    throw POSIXError(ready == 0 ? .ETIMEDOUT : .EIO)
                }
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
        }
    }
}
