import Foundation
import Darwin
import NativePipeProtocol

/// Only these selected items are visible to the mounted filesystem. Source
/// Unselected paths and transport credentials remain in their owning process.
public struct SharedFileVolumeDescriptor: Codable, Sendable {
    public let id: UUID
    public let name: String
    public let token: UUID
    public init(id: UUID, name: String, token: UUID) {
        self.id = id; self.name = name; self.token = token
    }
}

public struct SharedFileItem: Codable, Hashable, Sendable {
    public enum Kind: String, Codable, Sendable { case file, directory }
    public let id: UInt64
    public let parentID: UInt64
    public let name: String
    public let kind: Kind
    public let size: UInt64
    public let modified: Date?
    public let permissions: UInt32
    public let version: Data
    public init(id: UInt64, parentID: UInt64, name: String, kind: Kind, size: UInt64,
                modified: Date?, permissions: UInt32, version: Data) {
        self.id = id; self.parentID = parentID; self.name = name; self.kind = kind
        self.size = size; self.modified = modified; self.permissions = permissions
        self.version = version
    }
}

enum SharedFileOperation: Codable, Sendable {
    case item(UInt64), children(UInt64), read(UInt64, UInt64, Int, Data)
}
struct SharedFileRequest: Codable, Sendable {
    let token: UUID
    let operation: SharedFileOperation
}
struct SharedFileReply: Codable, Sendable {
    var item: SharedFileItem?
    var items: [SharedFileItem]?
    var data: Data?
    var more = false
    var error: Int32?
}

/// A bounded range protocol over the existing nonblocking local socket layer.
/// No remote credential or unselected path crosses the extension boundary.
public final class SharedFileVolumeClient: Sendable {
    public static let maximumReadLength = UserFileRange.maximumReadLength
    public let descriptor: SharedFileVolumeDescriptor
    private let socket: URL
    public init(directory: URL) throws {
        let manifest = directory.appendingPathComponent("volume.json")
        let fd = Darwin.open(manifest.path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        let handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        defer { try? handle.close() }
        var info = stat()
        guard fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFREG,
              info.st_uid == getuid(), info.st_size > 0, info.st_size <= 16 * 1024 else {
            throw POSIXError(.EACCES)
        }
        descriptor = try JSONDecoder().decode(SharedFileVolumeDescriptor.self,
            from: handle.read(upToCount: 16 * 1024) ?? Data())
        guard !descriptor.name.isEmpty else { throw POSIXError(.EINVAL) }
        socket = directory.appendingPathComponent("files.sock")
    }
    public func item(id: UInt64) async throws -> SharedFileItem {
        let replies = try await request(.item(id))
        guard replies.count == 1, let reply = replies.first,
              let item = reply.item, item.id == id, reply.items == nil, reply.data == nil else { throw POSIXError(.EPROTO) }
        return item
    }
    public func children(id: UInt64) async throws -> [SharedFileItem] {
        let replies = try await request(.children(id))
        var items: [SharedFileItem] = [], ids = Set<UInt64>(), names = Set<Data>()
        for reply in replies {
            guard let page = reply.items, reply.item == nil, reply.data == nil else { throw POSIXError(.EPROTO) }
            for item in page {
                guard item.id > 2, item.parentID == id, ids.insert(item.id).inserted,
                      names.insert(Data(item.name.utf8)).inserted, !item.name.isEmpty,
                      item.name != ".", item.name != "..", !item.name.contains("/"),
                      !item.name.contains("\0"), item.name.utf8.count <= 255 else { throw POSIXError(.EPROTO) }
            }
            items.append(contentsOf: page)
        }
        return items
    }
    public func read(id: UInt64, offset: UInt64, length: Int, version: Data) async throws -> Data {
        guard length >= 0, length <= Self.maximumReadLength,
              offset <= UInt64(Int64.max), UInt64(length) <= UInt64(Int64.max) - offset,
              !version.isEmpty, version.count <= UserFileRange.maximumVersionLength else { throw POSIXError(.EINVAL) }
        let replies = try await request(.read(id, offset, length, version))
        guard replies.count == 1, let reply = replies.first,
              let data = reply.data, data.count <= length, reply.item == nil, reply.items == nil else { throw POSIXError(.EPROTO) }
        return data
    }
    private func request(_ operation: SharedFileOperation) async throws -> [SharedFileReply] {
        let connection = try await SocketConnection.connect(to: socket)
        defer { connection.close() }
        try await FileRPC.sendRecord(try JSONEncoder().encode(SharedFileRequest(token: descriptor.token, operation: operation)),
                                     to: connection, deadline: .now() + .seconds(5))
        var replies: [SharedFileReply] = []
        var metadataBytes = 0
        repeat {
            try Task.checkCancellation()
            let data = try await FileRPC.receiveRecord(from: connection, maximum: 2 * 1024 * 1024,
                                                      deadline: .now() + .seconds(60))
            let reply = try JSONDecoder().decode(SharedFileReply.self, from: data)
            if let error = reply.error { throw POSIXError(POSIXErrorCode(rawValue: error) ?? .EIO) }
            if case .children = operation {
                metadataBytes += data.count
                guard metadataBytes <= 16 * 1024 * 1024, reply.items?.isEmpty == false || !reply.more else { throw POSIXError(.EOVERFLOW) }
            } else if reply.more { throw POSIXError(.EPROTO) }
            replies.append(reply)
        } while replies.last?.more == true
        return replies
    }
}
