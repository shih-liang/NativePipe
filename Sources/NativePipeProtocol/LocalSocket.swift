import Foundation
import Darwin

/// Shared local IPC listener. Parsing, business requests and long-lived
/// sessions use SocketConnection; idle peers occupy no blocking workers.
@MainActor
public final class LocalSocketServer {
    public struct Statistics: Sendable {
        public var accepted = 0, completed = 0, active = 0
    }
    private let url: URL
    private let maximumConnections: Int
    private var run: Run?
    public var isListening: Bool {
        guard let run else { return false }
        return run.queue.sync { run.listening }
    }
    public var statistics: Statistics {
        guard let run else { return Statistics() }
        return run.queue.sync { run.statistics }
    }

    public init(url: URL, maximumConnections: Int = 128) {
        precondition(maximumConnections > 0)
        self.url = url; self.maximumConnections = maximumConnections
    }
    deinit { run?.stop() }

    public func start(receive: @escaping @Sendable (SocketConnection) async -> Void) throws {
        guard !isListening else { return }
        var address = try LocalSocket.address(url)
        stop()
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let lock = open(url.appendingPathExtension("lock").path, O_CREAT | O_RDWR | O_CLOEXEC, 0o600)
        guard lock >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        guard flock(lock, LOCK_EX | LOCK_NB) == 0 else {
            let code = errno; close(lock); throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
        }
        var fd: Int32 = -1
        do {
            _ = unlink(url.path)
            fd = socket(AF_UNIX, SOCK_STREAM, 0)
            guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
            var noSignal: Int32 = 1
            guard fcntl(fd, F_SETFD, FD_CLOEXEC) == 0,
                  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0,
                  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal,
                             socklen_t(MemoryLayout.size(ofValue: noSignal))) == 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            guard withUnsafePointer(to: &address, {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
                }
            }) == 0, listen(fd, Int32(clamping: maximumConnections)) == 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            run = Run(fd: fd, ownership: lock, url: url,
                      maximumConnections: maximumConnections, receive: receive)
        } catch {
            if fd >= 0 { close(fd) }
            _ = unlink(url.path)
            flock(lock, LOCK_UN); close(lock)
            throw error
        }
    }

    /// Unpublish before a retirement acknowledgement, without truncating it.
    public func stopAccepting() { run?.stopAccepting() }
    public func stop() { let previous = run; run = nil; previous?.stop() }
    public func stopAndWait() async {
        let previous = run; run = nil
        await previous?.stopAndWait()
    }

    private final class Run: @unchecked Sendable {
        let queue = DispatchQueue(label: "com.nativepipe.local-listener", qos: .userInitiated)
        let fd: Int32, ownership: Int32, url: URL, maximumConnections: Int
        let receive: @Sendable (SocketConnection) async -> Void
        private var source: DispatchSourceRead!
        private var suspended = false
        var listening = true
        var statistics = Statistics()
        private var clients: [UUID: (SocketConnection, Task<Void, Never>)] = [:]
        private let closedListener = DispatchGroup()

        init(fd: Int32, ownership: Int32, url: URL, maximumConnections: Int,
             receive: @escaping @Sendable (SocketConnection) async -> Void) {
            self.fd = fd; self.ownership = ownership; self.url = url
            self.maximumConnections = maximumConnections; self.receive = receive
            source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
            source.setEventHandler { [weak self] in self?.acceptReady() }
            closedListener.enter()
            let closedListener = closedListener
            source.setCancelHandler { Darwin.close(fd); closedListener.leave() }
            source.activate()
        }

        private func acceptReady() {
            guard listening else { return }
            var budget = 64
            while clients.count < maximumConnections && budget > 0 {
                let descriptor = accept(fd, nil, nil)
                if descriptor < 0 {
                    switch errno {
                    case EINTR, ECONNABORTED: continue
                    case EAGAIN: return
                    case EMFILE, ENFILE, ENOMEM, ENOBUFS:
                        pause()
                        queue.asyncAfter(deadline: .now() + .milliseconds(50)) { [weak self] in self?.resume() }
                        return
                    default:
                        let code = errno
                        fputs("Local IPC listener failed: \(String(cString: strerror(code)))\n", stderr)
                        stopAcceptingOnQueue()
                        return
                    }
                }
                budget -= 1
                do {
                    let connection = try SocketConnection(owning: descriptor)
                    let id = UUID(), receive = receive
                    statistics.accepted += 1; statistics.active += 1
                    let task = Task { [self] in
                        await receive(connection)
                        connection.close()
                        await connection.waitUntilClosed()
                        queue.async { [self] in
                            clients.removeValue(forKey: id)
                            statistics.completed += 1; statistics.active -= 1
                            resume()
                        }
                    }
                    clients[id] = (connection, task)
                } catch {
                    // SocketConnection owns the descriptor even when setup fails.
                    fputs("Local IPC connection failed: \(error.localizedDescription)\n", stderr)
                }
            }
            if clients.count >= maximumConnections { pause() }
        }
        private func pause() {
            guard listening, !suspended else { return }
            suspended = true; source.suspend()
        }
        private func resume() {
            guard listening, suspended, clients.count < maximumConnections else { return }
            suspended = false; source.resume()
        }
        private func stopAcceptingOnQueue() {
            guard listening else { return }
            listening = false
            if suspended { suspended = false; source.resume() }
            source.cancel()
            // Never let an old cancellation callback unlink a replacement.
            _ = unlink(url.path)
            flock(ownership, LOCK_UN); Darwin.close(ownership)
        }
        func stopAccepting() { queue.sync { stopAcceptingOnQueue() } }
        func stop() {
            queue.sync {
                stopAcceptingOnQueue()
                for (connection, task) in clients.values { task.cancel(); connection.close() }
            }
        }
        func stopAndWait() async {
            let connections = queue.sync {
                stopAcceptingOnQueue()
                let connections = clients.values.map(\.0)
                for (connection, task) in clients.values { task.cancel(); connection.close() }
                return connections
            }
            for connection in connections { await connection.waitUntilClosed() }
            await withCheckedContinuation { continuation in
                closedListener.notify(queue: queue) { continuation.resume() }
            }
        }
    }
}

public enum LocalSocket {
    static func address(_ url: URL) throws -> sockaddr_un {
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        let size = MemoryLayout.size(ofValue: address.sun_path)
        guard url.path.utf8.count < size else { throw POSIXError(.ENAMETOOLONG) }
        url.path.withCString { source in
            withUnsafeMutablePointer(to: &address.sun_path) {
                $0.withMemoryRebound(to: CChar.self, capacity: size) { _ = strlcpy($0, source, size) }
            }
        }
        return address
    }

    /// Compatibility for synchronous CLI callers. GUI callers use SocketConnection.
    public static func connect(_ url: URL) throws -> FileHandle {
        var address = try address(url)
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        var noSignal: Int32 = 1
        guard fcntl(fd, F_SETFD, FD_CLOEXEC) == 0,
              setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal,
                         socklen_t(MemoryLayout.size(ofValue: noSignal))) == 0 else {
            let code = errno; close(fd); throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
        }
        let result = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard result == 0 else { let code = errno; close(fd); throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO) }
        return FileHandle(fileDescriptor: fd, closeOnDealloc: true)
    }
}
