import Foundation
import Darwin

/// Length-prefixed records between local processes: length:u32 (LE), then the
/// bytes. The receiver chooses a bound; an empty record is a valid record.
public enum RecordWire {
    public static func send(_ data: Data, to connection: SocketConnection, deadline: DispatchTime) async throws {
        guard data.count <= Int(UInt32.max) else { throw FileRPC.Failure.tooLarge }
        try await connection.write(header(data.count) + data, deadline: deadline)
    }

    public static func receive(from connection: SocketConnection, maximum: Int,
                               deadline: DispatchTime) async throws -> Data {
        let length = try count(try await connection.readExactly(4, deadline: deadline), maximum: maximum)
        return length == 0 ? Data() : Data(try await connection.readExactly(length, deadline: deadline))
    }

    public static func send(_ data: Data, to handle: FileHandle) throws {
        guard data.count <= Int(UInt32.max) else { throw FileRPC.Failure.tooLarge }
        try handle.write(contentsOf: header(data.count) + data)
    }

    public static func receive(from handle: FileHandle, maximum: Int) throws -> Data {
        let length = try count(try read(handle, 4), maximum: maximum)
        return try read(handle, length)
    }

    private static func header(_ count: Int) -> Data {
        withUnsafeBytes(of: UInt32(count).littleEndian) { Data($0) }
    }
    private static func count(_ header: Data, maximum: Int) throws -> Int {
        let length = Int(header.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(as: UInt32.self)) })
        guard maximum >= 0, length <= maximum else { throw FileRPC.Failure.protocolError }
        return length
    }
    private static func read(_ handle: FileHandle, _ count: Int) throws -> Data {
        var data = Data()
        while data.count < count {
            guard let chunk = try handle.read(upToCount: count - data.count), !chunk.isEmpty else {
                throw FileRPC.Failure.protocolError
            }
            data.append(chunk)
        }
        return data
    }
}
