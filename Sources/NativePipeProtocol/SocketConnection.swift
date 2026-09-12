import Foundation
import Darwin

/// A duplex, nonblocking byte stream. One serial queue owns its descriptor,
/// pending operations and deadlines; idle sockets occupy no worker threads.
public final class SocketConnection: @unchecked Sendable {
    public enum Failure: Error, Equatable { case endOfStream, truncated }
    private let state: State

    /// Takes ownership, including on failure. Only this connection may read,
    /// write or close the descriptor after adoption.
    public init(owning descriptor: Int32, maximumQueuedBytes: Int = 64 * 1024 * 1024) throws {
        precondition(maximumQueuedBytes > 0)
        guard fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0,
              fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK) == 0 else {
            let code = errno; Darwin.close(descriptor); throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
        }
        state = State(fd: descriptor, maximumQueuedBytes: maximumQueuedBytes)
    }

    deinit { close() }

    public static func connect(to url: URL, deadline: DispatchTime = .now() + .seconds(5)) async throws -> SocketConnection {
        let address = try LocalSocket.address(url)
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        var noSignal: Int32 = 1
        guard setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal,
                         socklen_t(MemoryLayout.size(ofValue: noSignal))) == 0 else {
            let code = errno; Darwin.close(fd); throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
        }
        let connection = try SocketConnection(owning: fd)
        do {
            try await connection.state.connect(address: address, deadline: deadline)
            return connection
        } catch { connection.close(); throw error }
    }

    public func read(upToCount count: Int, deadline: DispatchTime? = nil) async throws -> Data {
        precondition(count > 0)
        return try await withTaskCancellationHandler {
            try Task.checkCancellation()
            return try await withCheckedThrowingContinuation { continuation in
                state.queue.async { self.state.beginRead(count: count, deadline: deadline, continuation: continuation) }
            }
        } onCancel: { self.close() }
    }

    public func readExactly(_ count: Int, deadline: DispatchTime? = nil) async throws -> Data {
        precondition(count >= 0)
        var data = Data()
        while data.count < count {
            let chunk = try await read(upToCount: min(count - data.count, 64 * 1024), deadline: deadline)
            guard !chunk.isEmpty else { throw data.isEmpty ? Failure.endOfStream : Failure.truncated }
            data.append(chunk)
        }
        return data
    }

    public func write(_ data: Data, deadline: DispatchTime? = nil) async throws {
        try await withTaskCancellationHandler {
            try Task.checkCancellation()
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                send(data, deadline: deadline) { continuation.resume(with: $0) }
            }
        } onCancel: { self.close() }
    }

    /// Ordered, bounded output for synchronous event producers such as a console.
    /// A slow consumer fails instead of accumulating unlimited suspended tasks.
    public func send(_ data: Data, deadline: DispatchTime? = nil,
                     completion: @escaping @Sendable (Result<Void, Error>) -> Void) {
        let state = state
        // Reserve before dispatching: queued callbacks also retain their Data.
        // Counting only bytes already in the event loop leaves that queue unbounded.
        if let error = state.reserveOutput(data.count) {
            state.queue.async { state.close(error: error); completion(.failure(error)) }
        } else {
            state.queue.async { state.beginWrite(data, deadline: deadline, completion: completion) }
        }
    }

    public func finishReading() { state.queue.async { self.state.finishReading() } }
    public func finishWriting() { state.queue.async { self.state.finishWriting() } }
    public func close() {
        let state = state
        state.queue.async { state.close(error: CancellationError()) }
    }
    public func processIdentifier() async throws -> pid_t {
        try await withCheckedThrowingContinuation { continuation in
            state.queue.async { self.state.processIdentifier(continuation) }
        }
    }
    public func waitUntilClosed() async {
        await withCheckedContinuation { continuation in
            state.queue.async {
                if self.state.descriptorClosed { continuation.resume() }
                else { self.state.closeWaiters.append(continuation) }
            }
        }
    }

    /// Explicit ownership transfer for the synchronous CLI's terminal loop.
    /// Exact reads never consume bytes belonging to the upgraded stream.
    public func detachFileHandle() async throws -> FileHandle {
        let descriptor = try await withCheckedThrowingContinuation { continuation in
            state.queue.async { self.state.detach(continuation) }
        }
        await waitUntilClosed()
        let handle = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        if Task.isCancelled { try? handle.close(); throw CancellationError() }
        return handle
    }

    /// A boundary adapter for synchronous CLI APIs, never used by GUI requests.
    public static func blocking<T: Sendable>(
        _ operation: @escaping @Sendable () async throws -> T
    ) throws -> T {
        let result = BlockingResult<T>()
        Task.detached {
            do { result.value = .success(try await operation()) }
            catch { result.value = .failure(error) }
            result.ready.signal()
        }
        result.ready.wait()
        return try result.value!.get()
    }

    private final class BlockingResult<T>: @unchecked Sendable {
        let ready = DispatchSemaphore(value: 0)
        var value: Result<T, Error>?
    }

    private final class State: @unchecked Sendable {
        let queue = DispatchQueue(label: "com.nativepipe.socket", qos: .userInitiated)
        let fd: Int32
        let maximumQueuedBytes: Int
        private var readSource: DispatchSourceRead!
        private var writeSource: DispatchSourceWrite!
        private var timer: DispatchSourceTimer!
        private var readSuspended = true, writeSuspended = true
        private var reading: (count: Int, deadline: DispatchTime?, continuation: CheckedContinuation<Data, Error>)?
        private struct Write {
            let data: Data
            var offset = 0
            let deadline: DispatchTime?
            let completion: @Sendable (Result<Void, Error>) -> Void
        }
        private var writes: [Write] = []
        private let outputLock = NSLock()
        private var reservedOutputBytes = 0
        private var acceptsOutput = true
        private var connecting: (deadline: DispatchTime, continuation: CheckedContinuation<Void, Error>)?
        private var closed = false, inputEnded = false, outputEnded = false
        private var cancellations = 0
        var descriptorClosed = false
        var closeWaiters: [CheckedContinuation<Void, Never>] = []

        init(fd: Int32, maximumQueuedBytes: Int) {
            self.fd = fd; self.maximumQueuedBytes = maximumQueuedBytes
            // Initialization precedes publication; all later state stays on queue.
            readSource = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
            writeSource = DispatchSource.makeWriteSource(fileDescriptor: fd, queue: queue)
            timer = DispatchSource.makeTimerSource(queue: queue)
            readSource.setEventHandler { [weak self] in self?.readReady() }
            writeSource.setEventHandler { [weak self] in self?.writeReady() }
            // Cancellation handlers retain the state until both sources can no
            // longer access fd. Closing earlier risks using a recycled descriptor.
            readSource.setCancelHandler { self.cancelledSource() }
            writeSource.setCancelHandler { self.cancelledSource() }
            timer.setEventHandler { [weak self] in self?.expired() }
            timer.schedule(deadline: .distantFuture)
            readSource.activate(); readSource.suspend()
            writeSource.activate(); writeSource.suspend()
            timer.activate()
        }

        func connect(address: sockaddr_un, deadline: DispatchTime) async throws {
            try await withTaskCancellationHandler {
                try Task.checkCancellation()
                try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                    queue.async {
                        guard !self.closed else { continuation.resume(throwing: CancellationError()); return }
                        guard !self.isExpired(deadline) else {
                            continuation.resume(throwing: POSIXError(.ETIMEDOUT)); return
                        }
                        var address = address
                        let result = withUnsafePointer(to: &address) {
                            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                                Darwin.connect(self.fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
                            }
                        }
                        if result == 0 { continuation.resume(); return }
                        guard errno == EINPROGRESS else {
                            continuation.resume(throwing: POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)); return
                        }
                        self.connecting = (deadline, continuation)
                        self.watchWrite(true); self.scheduleTimer()
                    }
                }
            } onCancel: { self.queue.async { self.close(error: CancellationError()) } }
        }

        func beginRead(count: Int, deadline: DispatchTime?, continuation: CheckedContinuation<Data, Error>) {
            guard !closed else { continuation.resume(throwing: CancellationError()); return }
            guard reading == nil else { continuation.resume(throwing: POSIXError(.EBUSY)); return }
            if inputEnded { continuation.resume(returning: Data()); return }
            reading = (count, deadline, continuation)
            if isExpired(deadline) { close(error: POSIXError(.ETIMEDOUT)); return }
            watchRead(true); readReady(); scheduleTimer()
        }

        private func readReady() {
            guard !closed, let operation = reading else { return }
            if isExpired(operation.deadline) { close(error: POSIXError(.ETIMEDOUT)); return }
            var data = Data(count: min(operation.count, 64 * 1024))
            var count: Int
            repeat { count = data.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) } }
            while count < 0 && errno == EINTR
            if count < 0 {
                if errno == EAGAIN || errno == EWOULDBLOCK { return }
                close(error: POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)); return
            }
            reading = nil; watchRead(false)
            data.count = count
            if count == 0 { inputEnded = true }
            operation.continuation.resume(returning: data)
            scheduleTimer()
        }

        func beginWrite(_ data: Data, deadline: DispatchTime?,
                        completion: @escaping @Sendable (Result<Void, Error>) -> Void) {
            guard !closed, !outputEnded else {
                releaseOutput(data.count); completion(.failure(POSIXError(.EPIPE))); return
            }
            if data.isEmpty { completion(.success(())); return }
            writes.append(Write(data: data, deadline: deadline, completion: completion))
            if isExpired(deadline) { close(error: POSIXError(.ETIMEDOUT)); return }
            watchWrite(true); writeReady(); scheduleTimer()
        }

        private func writeReady() {
            guard !closed else { return }
            if isExpired(connecting?.deadline) || writes.contains(where: { isExpired($0.deadline) }) {
                close(error: POSIXError(.ETIMEDOUT)); return
            }
            if let pending = connecting {
                var error: Int32 = 0, size = socklen_t(MemoryLayout<Int32>.size)
                guard getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0, error == 0 else {
                    close(error: POSIXError(POSIXErrorCode(rawValue: error == 0 ? errno : error) ?? .EIO)); return
                }
                connecting = nil
                pending.continuation.resume()
            }
            // Bound work per callback so other sockets, cancellation and timers
            // keep progressing even when this peer continuously drains output.
            var budget = 256 * 1024
            while !writes.isEmpty && budget > 0 {
                let operation = writes[0]
                let count = operation.data.withUnsafeBytes {
                    Darwin.send(fd, $0.baseAddress!.advanced(by: operation.offset),
                                min($0.count - operation.offset, budget), MSG_NOSIGNAL)
                }
                if count < 0 {
                    if errno == EINTR { continue }
                    if errno == EAGAIN || errno == EWOULDBLOCK { break }
                    close(error: POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)); return
                }
                guard count > 0 else { close(error: POSIXError(.EPIPE)); return }
                writes[0].offset += count; budget -= count
                if writes[0].offset == operation.data.count {
                    releaseOutput(operation.data.count)
                    writes.removeFirst().completion(.success(()))
                }
            }
            watchWrite(connecting != nil || !writes.isEmpty)
            scheduleTimer()
        }

        func finishReading() {
            guard !closed, !inputEnded else { return }
            inputEnded = true; _ = shutdown(fd, SHUT_RD)
            let operation = reading; reading = nil; watchRead(false)
            operation?.continuation.resume(returning: Data())
            scheduleTimer()
        }

        func processIdentifier(_ continuation: CheckedContinuation<pid_t, Error>) {
            guard !closed else { continuation.resume(throwing: CancellationError()); return }
            var pid: pid_t = 0, size = socklen_t(MemoryLayout<pid_t>.size)
            if getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &size) == 0, pid > 0 {
                continuation.resume(returning: pid)
            } else { continuation.resume(throwing: POSIXError(.ENOTCONN)) }
        }

        func finishWriting() {
            guard !closed, !outputEnded else { return }
            // Called only after the producer has awaited its final write.
            guard writes.isEmpty else { close(error: POSIXError(.EBUSY)); return }
            outputEnded = true; _ = shutdown(fd, SHUT_WR)
        }

        func reserveOutput(_ count: Int) -> POSIXError? {
            outputLock.lock(); defer { outputLock.unlock() }
            guard acceptsOutput else { return POSIXError(.EPIPE) }
            guard count <= maximumQueuedBytes - reservedOutputBytes else {
                acceptsOutput = false; return POSIXError(.ENOBUFS)
            }
            reservedOutputBytes += count
            return nil
        }
        private func releaseOutput(_ count: Int) {
            outputLock.lock(); reservedOutputBytes -= count; outputLock.unlock()
        }

        private func isExpired(_ deadline: DispatchTime?) -> Bool {
            deadline.map { $0 <= .now() } ?? false
        }
        private func scheduleTimer() {
            guard !closed else { return }
            let deadlines = [reading?.deadline, connecting?.deadline].compactMap { $0 }
                + writes.compactMap(\.deadline)
            timer.schedule(deadline: deadlines.min() ?? .distantFuture, leeway: .milliseconds(1))
        }
        private func expired() {
            guard !closed else { return }
            if isExpired(reading?.deadline) || isExpired(connecting?.deadline)
                || writes.contains(where: { isExpired($0.deadline) }) {
                close(error: POSIXError(.ETIMEDOUT))
            } else { scheduleTimer() }
        }
        private func watchRead(_ enabled: Bool) {
            if enabled && readSuspended { readSuspended = false; readSource.resume() }
            else if !enabled && !readSuspended { readSuspended = true; readSource.suspend() }
        }
        private func watchWrite(_ enabled: Bool) {
            if enabled && writeSuspended { writeSuspended = false; writeSource.resume() }
            else if !enabled && !writeSuspended { writeSuspended = true; writeSource.suspend() }
        }
        func detach(_ continuation: CheckedContinuation<Int32, Error>) {
            guard !closed, reading == nil, writes.isEmpty, connecting == nil else {
                continuation.resume(throwing: POSIXError(.EBUSY)); return
            }
            let descriptor = fcntl(fd, F_DUPFD_CLOEXEC, 0)
            guard descriptor >= 0 else {
                continuation.resume(throwing: POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)); return
            }
            guard fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) & ~O_NONBLOCK) == 0 else {
                let code = errno; Darwin.close(descriptor)
                continuation.resume(throwing: POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)); return
            }
            close(error: CancellationError(), disconnect: false)
            continuation.resume(returning: descriptor)
        }
        func close(error: Error, disconnect: Bool = true) {
            guard !closed else { return }
            closed = true
            outputLock.lock(); acceptsOutput = false; outputLock.unlock()
            let read = reading, connect = connecting, output = writes
            reading = nil; connecting = nil; writes = []
            if disconnect {
                // Darwin can reject SHUT_RDWR with ENOTCONN after a peer's
                // write-half closes, leaving our output open on duplicated
                // descriptors. Finish output independently before input.
                _ = shutdown(fd, SHUT_WR)
                _ = shutdown(fd, SHUT_RD)
            }
            if readSuspended { readSuspended = false; readSource.resume() }
            if writeSuspended { writeSuspended = false; writeSource.resume() }
            readSource.cancel(); writeSource.cancel(); timer.cancel()
            read?.continuation.resume(throwing: error)
            connect?.continuation.resume(throwing: error)
            output.forEach { releaseOutput($0.data.count); $0.completion(.failure(error)) }
        }
        private func cancelledSource() {
            cancellations += 1
            guard cancellations == 2 else { return }
            Darwin.close(fd); descriptorClosed = true
            readSource = nil; writeSource = nil; timer = nil
            let waiters = closeWaiters; closeWaiters = []
            waiters.forEach { $0.resume() }
        }
    }
}
