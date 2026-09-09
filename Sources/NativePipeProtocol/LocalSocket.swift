import Foundation
import Darwin

/// The low-frequency app/helper IPC listener. Rendering and SSH streams do not
/// cross this socket. Ownership and disconnected-peer handling come from VMHost.
@MainActor
public final class LocalSocketServer {
    private let url: URL
    private var descriptor: Int32 = -1
    private var ownership: Int32 = -1
    private let queue = DispatchQueue(label: "com.nativepipe.local-clients", attributes: .concurrent)
    public init(url: URL) { self.url = url }

    public func start(receive: @escaping @Sendable (FileHandle) -> Void) throws {
        guard descriptor < 0 else { return }
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let lock = open(url.appendingPathExtension("lock").path, O_CREAT | O_RDWR | O_CLOEXEC, 0o600)
        guard lock >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        guard flock(lock, LOCK_EX | LOCK_NB) == 0 else {
            let code = errno; close(lock)
            throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
        }
        ownership = lock
        do {
            try? FileManager.default.removeItem(at: url)
            let fd = socket(AF_UNIX, SOCK_STREAM, 0)
            guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
            descriptor = fd
            _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
            var address = try LocalSocket.address(url)
            guard withUnsafePointer(to: &address, {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
                }
            }) == 0, listen(fd, 16) == 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            let queue = queue
            let acceptFD = fcntl(fd, F_DUPFD_CLOEXEC, 0)
            guard acceptFD >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
            queue.async {
                defer { close(acceptFD) }
                while true {
                    let client = accept(acceptFD, nil, nil)
                    if client < 0 {
                        if errno == EINTR { continue }
                        return
                    }
                    _ = fcntl(client, F_SETFD, FD_CLOEXEC)
                    var noSignal: Int32 = 1
                    guard setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSignal,
                                    socklen_t(MemoryLayout.size(ofValue: noSignal))) == 0 else {
                        close(client); continue
                    }
                    let handle = FileHandle(fileDescriptor: client, closeOnDealloc: true)
                    queue.async { receive(handle) }
                }
            }
        } catch { stop(); throw error }
    }

    public func stop() {
        guard ownership >= 0 else { return }
        if descriptor >= 0 {
            shutdown(descriptor, SHUT_RDWR)
            close(descriptor)
            descriptor = -1
        }
        try? FileManager.default.removeItem(at: url)
        flock(ownership, LOCK_UN)
        close(ownership)
        ownership = -1
    }
}

public enum LocalSocket {
    fileprivate static func address(_ url: URL) throws -> sockaddr_un {
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        let size = MemoryLayout.size(ofValue: address.sun_path)
        guard url.path.utf8.count < size else { throw POSIXError(.ENAMETOOLONG) }
        url.path.withCString { source in
            withUnsafeMutablePointer(to: &address.sun_path) {
                $0.withMemoryRebound(to: CChar.self, capacity: size) {
                    _ = strlcpy($0, source, size)
                }
            }
        }
        return address
    }
    public static func connect(_ url: URL) throws -> FileHandle {
        var address = try address(url)
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
        var noSignal: Int32 = 1
        guard setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal,
                        socklen_t(MemoryLayout.size(ofValue: noSignal))) == 0 else {
            let code = errno; close(fd)
            throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
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
