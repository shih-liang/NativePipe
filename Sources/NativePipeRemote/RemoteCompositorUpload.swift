import CryptoKit
import Darwin
import Foundation

/// The bounded SSH prelude ends before RemoteStreamDecoder receives any bytes.
/// All disk hashing and pipe I/O run on the connection reader, off the UI actor.
enum RemoteCompositorUpload {
    static let maximumArchiveSize = 512 * 1024 * 1024

    static func prepare(directory: URL, input: FileHandle, output: FileHandle,
                        write: (FileHandle, Data) throws -> Void) throws -> Bool {
        guard let target = try readLine(output) else { return false }
        let fields = target.split(separator: " ")
        guard fields.count == 4, fields[0] == "NATIVEPIPE", fields[1] == "TARGET",
              ["aarch64", "x86_64"].contains(fields[2]), ["gnu", "musl"].contains(fields[3]) else {
            throw RemoteError.message("Invalid NativePipe installation response: \(target)")
        }
        let name = "nativepipe-compositor-\(fields[2])-\(fields[3]).tar.gz"
        let archive: FileHandle
        do { archive = try FileHandle(forReadingFrom: directory.appendingPathComponent(name)) }
        catch { throw RemoteError.message("This FluxWindow build is missing its remote compositor package: \(name).") }
        defer { try? archive.close() }
        var hash = SHA256(), size = 0
        while let chunk = try archive.read(upToCount: 1_048_576), !chunk.isEmpty {
            size += chunk.count
            guard size <= maximumArchiveSize else { throw RemoteError.message("NativePipe archive exceeds the size limit.") }
            hash.update(data: chunk)
        }
        guard size > 0 else { throw RemoteError.message("NativePipe archive is empty: \(name).") }
        let digest = hash.finalize().map { String(format: "%02x", $0) }.joined()
        try write(input, Data("\(digest) \(size)\n".utf8))
        guard let response = try readLine(output) else { return false }
        switch response {
        case "NATIVEPIPE CACHED": return true
        case "NATIVEPIPE UPLOAD":
            try archive.seek(toOffset: 0)
            let deadline = ProcessInfo.processInfo.systemUptime + 300
            var remaining = size
            while remaining > 0 {
                guard ProcessInfo.processInfo.systemUptime < deadline else {
                    throw RemoteError.message("NativePipe upload timed out.")
                }
                guard let chunk = try archive.read(upToCount: min(65_536, remaining)), !chunk.isEmpty else {
                    throw RemoteError.message("NativePipe archive changed during upload.")
                }
                try write(input, chunk)
                remaining -= chunk.count
            }
            return true
        default: throw RemoteError.message("Invalid NativePipe upload response: \(response)")
        }
    }

    private static func readLine(_ handle: FileHandle) throws -> String? {
        var bytes = [UInt8]()
        let deadline = ProcessInfo.processInfo.systemUptime + 120
        // Read exactly through newline: the next byte may already be NPIP.
        while bytes.count < 256 {
            guard ProcessInfo.processInfo.systemUptime < deadline else {
                throw RemoteError.message("NativePipe installation handshake timed out.")
            }
            var fd = pollfd(fd: handle.fileDescriptor, events: Int16(POLLIN), revents: 0)
            let available = Darwin.poll(&fd, 1, 1000)
            if available == 0 || (available < 0 && errno == EINTR) { continue }
            guard available > 0 else { throw POSIXError(.EIO) }
            var byte: UInt8 = 0
            let count = Darwin.read(handle.fileDescriptor, &byte, 1)
            if count == 0 { return nil } // stderr/SSH exit status owns diagnostics.
            if count < 0 && errno == EINTR { continue }
            guard count == 1 else { throw POSIXError(.EIO) }
            if byte == 10 { return String(decoding: bytes, as: UTF8.self) }
            bytes.append(byte)
        }
        throw RemoteError.message("NativePipe installation response exceeds the size limit.")
    }
}
