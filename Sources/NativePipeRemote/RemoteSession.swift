import Foundation
import NativePipeProtocol
#if canImport(Darwin)
import Darwin
#endif

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

    private let lock = NSLock()
    private var surfaceFD: Int32 = -1
    private var mediaFD: Int32 = -1
    private var surfaceSource: DispatchSourceRead?
    private var mediaSource: DispatchSourceRead?
    private var surfaceDecoder = FrameDecoder()
    private let mediaDemuxer = MediaWire.Demuxer()
    private let readQueue = DispatchQueue(label: "com.nativepipe.remote.read", qos: .userInteractive)
    private let writeQueue = DispatchQueue(label: "com.nativepipe.remote.write", qos: .userInteractive)
    private var pendingWrites: [Windowing.HostCommand] = []
    private var writerScheduled = false

    public var onEvent: ((Windowing.GuestEvent) -> Void)?
    public var onMediaFrame: ((MediaWire.Header, Data) -> Void)?
    public var onStateChange: ((State) -> Void)?

    public init(
        host: String = "127.0.0.1",
        surfacePort: UInt16 = UInt16(NativePipePort.surface),
        mediaPort: UInt16 = UInt16(NativePipePort.media)
    ) {
        self.host = host
        self.surfacePort = surfacePort
        self.mediaPort = mediaPort
    }

    public var isConnected: Bool {
        lock.lock(); defer { lock.unlock() }
        return surfaceFD >= 0
    }

    /// Connect both channels. Fails if either TCP connection cannot be opened.
    public func connect() throws {
        disconnect()
        let surface = try Self.openTCP(host: host, port: surfacePort)
        let media = try Self.openTCP(host: host, port: mediaPort)
        lock.lock()
        surfaceFD = surface
        mediaFD = media
        surfaceDecoder = FrameDecoder()
        pendingWrites.removeAll(keepingCapacity: true)
        writerScheduled = false
        lock.unlock()
        armSurfaceReader(surface)
        armMediaReader(media)
        DispatchQueue.main.async { self.onStateChange?(.connected) }
    }

    public func disconnect() {
        lock.lock()
        let surface = surfaceFD
        let media = mediaFD
        let surfaceSource = self.surfaceSource
        let mediaSource = self.mediaSource
        surfaceFD = -1
        mediaFD = -1
        self.surfaceSource = nil
        self.mediaSource = nil
        pendingWrites.removeAll(keepingCapacity: true)
        lock.unlock()
        surfaceSource?.cancel()
        mediaSource?.cancel()
        if surface >= 0 { Darwin.close(surface) }
        if media >= 0 { Darwin.close(media) }
        DispatchQueue.main.async { self.onStateChange?(.disconnected) }
    }

    public func send(_ command: Windowing.HostCommand) {
        lock.lock()
        guard surfaceFD >= 0 else {
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
            guard !pendingWrites.isEmpty, surfaceFD >= 0 else {
                writerScheduled = false
                lock.unlock()
                return
            }
            let command = pendingWrites.removeFirst()
            let fd = surfaceFD
            lock.unlock()

            do {
                let payload: Data
                if let fast = WindowWire.fastPayload(for: command) {
                    payload = fast
                } else {
                    payload = try JSONEncoder().encode(command)
                }
                let framed = try WireFormat.frame(payload: payload)
                try Self.writeAll(fd: fd, data: framed)
            } catch {
                disconnect()
                return
            }
        }
    }

    private func armSurfaceReader(_ fd: Int32) {
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: readQueue)
        source.setEventHandler { [weak self] in
            self?.drainSurface()
        }
        lock.lock()
        surfaceSource?.cancel()
        surfaceSource = source
        lock.unlock()
        source.resume()
        readQueue.async { [weak self] in self?.drainSurface() }
    }

    private func armMediaReader(_ fd: Int32) {
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: readQueue)
        source.setEventHandler { [weak self] in
            self?.drainMedia()
        }
        lock.lock()
        mediaSource?.cancel()
        mediaSource = source
        lock.unlock()
        source.resume()
        readQueue.async { [weak self] in self?.drainMedia() }
    }

    private func drainSurface() {
        lock.lock()
        let fd = surfaceFD
        lock.unlock()
        guard fd >= 0 else { return }

        var buffer = [UInt8](repeating: 0, count: 256 * 1024)
        let n = Darwin.recv(fd, &buffer, buffer.count, 0)
        if n == 0 {
            fputs("nativepipe-remote: surface channel EOF\n", stderr)
            fflush(stderr)
            disconnect()
            return
        }
        if n < 0 {
            if errno == EAGAIN || errno == EWOULDBLOCK { return }
            fputs("nativepipe-remote: surface read errno=\(errno)\n", stderr)
            fflush(stderr)
            disconnect()
            return
        }
        let data = Data(buffer.prefix(n))
        lock.lock()
        surfaceDecoder.append(data)
        var events: [Windowing.GuestEvent] = []
        do {
            while let payload = try surfaceDecoder.next() {
                do {
                    if payload.starts(with: WindowWire.sceneMagic) ||
                        payload.starts(with: WindowWire.lifecycleMagic) {
                        events.append(try WindowWire.guestEvent(from: payload))
                    } else {
                        events.append(try JSONDecoder().decode(
                            Windowing.GuestEvent.self, from: payload))
                    }
                } catch {
                    fputs(
                        "nativepipe-remote: skip event: \(String(decoding: payload, as: UTF8.self))\n",
                        stderr)
                    fflush(stderr)
                }
            }
        } catch {
            lock.unlock()
            fputs("nativepipe-remote: surface framing error: \(error)\n", stderr)
            fflush(stderr)
            disconnect()
            return
        }
        lock.unlock()
        guard !events.isEmpty else { return }
        Task { @MainActor in
            for event in events { self.onEvent?(event) }
        }
    }

    private func drainMedia() {
        lock.lock()
        let fd = mediaFD
        lock.unlock()
        guard fd >= 0 else { return }

        var buffer = [UInt8](repeating: 0, count: 256 * 1024)
        let n = Darwin.recv(fd, &buffer, buffer.count, 0)
        if n == 0 {
            fputs("nativepipe-remote: media channel EOF\n", stderr)
            fflush(stderr)
            disconnect()
            return
        }
        if n < 0 {
            if errno == EAGAIN || errno == EWOULDBLOCK { return }
            fputs("nativepipe-remote: media read errno=\(errno)\n", stderr)
            fflush(stderr)
            disconnect()
            return
        }
        let frames = mediaDemuxer.push(Data(buffer.prefix(n)))
        guard !frames.isEmpty else { return }
        for (header, payload) in frames {
            onMediaFrame?(header, payload)
        }
    }

    private static func openTCP(host: String, port: UInt16) throws -> Int32 {
        let fd = Darwin.socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(port.bigEndian)
        let pton = host.withCString { cs in
            inet_pton(AF_INET, cs, &addr.sin_addr)
        }
        if pton != 1 {
            Darwin.close(fd)
            throw POSIXError(.EHOSTUNREACH)
        }
        let ok = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if ok != 0 {
            let code = errno
            Darwin.close(fd)
            throw POSIXError(POSIXErrorCode(rawValue: code) ?? .ECONNREFUSED)
        }
        let flags = fcntl(fd, F_GETFL)
        if flags >= 0 { _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK) }
        return fd
    }

    private static func writeAll(fd: Int32, data: Data) throws {
        try data.withUnsafeBytes { raw in
            var sent = 0
            let total = raw.count
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            while sent < total {
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
