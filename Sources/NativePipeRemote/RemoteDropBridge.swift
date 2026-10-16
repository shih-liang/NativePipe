import Foundation
import Darwin
import NativePipeProtocol

/// Dropped Mac files and folders for a remote computer, opened there in place
/// through the remote compositor's FUSE mount. The NPFR v2 session has no
/// connection of its own: its bytes ride the SSH stream as ordered NPFS
/// records on the bulk lane. The remote speaks first (only a compositor that
/// was told this Mac understands NPFS does), so nothing is ever sent to one
/// that would not understand it. Without a mount, drops are copied over SFTP.
public final class RemoteDropBridge: @unchecked Sendable {
    private let server: DropFileServer
    private let lock = NSLock()
    private var writer: WindowCommandWriter?
    private var pipe: Pipe?
    private var reclaiming: Task<Void, Never>?

    /// Our end of the socket pair whose other end the session owns. Closed by
    /// whichever of the reader, the pump or disconnect lets go of it last.
    private final class Pipe: @unchecked Sendable {
        let descriptor: Int32
        private let lock = NSLock()
        private var users = 1 // the pump
        private var closing = false
        init(_ descriptor: Int32) { self.descriptor = descriptor }
        func use() -> Bool { lock.withLock { guard !closing else { return false }; users += 1; return true } }
        func done() {
            let close = lock.withLock { users -= 1; return users == 0 }
            if close { Darwin.close(descriptor) }
        }
        func shut() {
            let first = lock.withLock { defer { closing = true }; return !closing }
            if first { shutdown(descriptor, SHUT_RDWR) }
        }
    }

    static let pumpChunk = 60 * 1024
    static let backlogLimit = 1024 * 1024

    public init(stagingDirectory: URL? = nil) {
        server = DropFileServer(stagingDirectory: stagingDirectory)
        // The same lifetime rules as a virtual machine's drops.
        reclaiming = Task.detached(priority: .utility) { [weak server] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 60_000_000_000)
                guard let server else { return }
                server.reclaim(idleLimit: 30 * 60, stagedLimit: 2 << 30)
            }
        }
    }

    deinit {
        reclaiming?.cancel()
        disconnect()
        server.removeAll()
    }

    public var isReady: Bool { server.isReady }

    /// The remote path for a dropped file or folder, or nil to copy it instead.
    public func share(_ url: URL) -> URL? { server.share(url) }

    /// A new SSH connection; the previous one's session, if any, is gone.
    func connect(writer: WindowCommandWriter) {
        disconnect()
        lock.withLock { self.writer = writer }
    }

    func disconnect() {
        let old: Pipe? = lock.withLock {
            defer { pipe = nil; writer = nil }
            return pipe
        }
        old?.shut()
        server.detach()
    }

    /// Session bytes from the remote, on the stream reader, in order.
    func receive(_ bytes: Data) {
        guard let pipe = currentPipe(), pipe.use() else { return }
        defer { pipe.done() }
        // The session's reader drains this socket continuously, and NPFR
        // credit bounds what the remote may have in flight.
        var offset = bytes.startIndex
        while offset < bytes.endIndex {
            let count = bytes[offset...].withUnsafeBytes { Darwin.send(pipe.descriptor, $0.baseAddress, $0.count, 0) }
            if count < 0 && errno == EINTR { continue }
            if count <= 0 { return }
            offset += count
        }
    }

    /// The remote's first bytes start the host side of the session.
    private func currentPipe() -> Pipe? {
        lock.lock(); defer { lock.unlock() }
        if let pipe { return pipe }
        guard let writer else { return nil }
        var descriptors: [Int32] = [-1, -1]
        guard socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors) == 0 else { return nil }
        var yes: Int32 = 1
        _ = setsockopt(descriptors[1], SOL_SOCKET, SO_NOSIGPIPE, &yes, 4)
        guard let connection = try? SocketConnection(owning: descriptors[0]) else {
            Darwin.close(descriptors[1]); return nil
        }
        server.attach(connection)
        let pipe = Pipe(descriptors[1])
        self.pipe = pipe
        Thread.detachNewThread { Self.pump(pipe, to: writer) }
        return pipe
    }

    /// Session bytes for the remote, paced by the bulk lane's backlog.
    private static func pump(_ pipe: Pipe, to writer: WindowCommandWriter) {
        defer { pipe.done() }
        var buffer = [UInt8](repeating: 0, count: pumpChunk)
        while true {
            let count = buffer.withUnsafeMutableBytes { Darwin.read(pipe.descriptor, $0.baseAddress, pumpChunk) }
            if count < 0 && errno == EINTR { continue }
            guard count > 0 else { return }
            while writer.fileSessionBacklog > backlogLimit { usleep(2000) }
            guard writer.sendFileSession(Data(buffer[0..<count])) else { return }
        }
    }
}
