import Foundation

/// Wire format of `np-open`, the guest tool that asks the Mac to open a URL or
/// a file. One request and one response per vsock connection, little-endian:
///
///     request:  "NPOP" | version u8 | kind u8 | reserved u16 = 0 | length u32 | payload
///     response: "NPOR" | version u8 | status u8 | reserved u16 = 0 | length u32 | message
///
/// The C side is guest/session/np-open-wire.h. Both are exercised against the
/// same golden bytes.
public enum HostOpenWire {
    public static let version: UInt8 = 1
    public static let headerSize = 12
    public static let maximumPayload = 8192
    public static let maximumMessage = 512

    public enum Kind: UInt8, Sendable, Equatable {
        case url = 1
        /// A path inside the guest.
        case file = 2
    }

    public enum Status: UInt8, Sendable, Equatable {
        case opened = 0
        /// The host's policy does not allow it.
        case refused = 1
        /// Allowed, but it could not be opened.
        case failed = 2
        /// Forwarding is switched off on the Mac.
        case disabled = 3
    }

    public struct Request: Sendable, Equatable {
        public var kind: Kind
        public var value: String
        public init(kind: Kind, value: String) { self.kind = kind; self.value = value }
    }

    public struct Response: Sendable, Equatable {
        public var status: Status
        public var message: String
        public init(status: Status, message: String = "") { self.status = status; self.message = message }
    }

    public enum DecodeError: Error, Equatable {
        case malformed
    }

    /// Validates a request header and returns how many payload bytes follow, so
    /// a reader can bound its next read before trusting anything else.
    public static func payloadLength(inRequestHeader header: Data) throws -> Int {
        guard header.count == headerSize else { throw DecodeError.malformed }
        let bytes = [UInt8](header)
        guard Array(bytes[0..<4]) == Array("NPOP".utf8), bytes[4] == version,
              Kind(rawValue: bytes[5]) != nil, bytes[6] == 0, bytes[7] == 0 else {
            throw DecodeError.malformed
        }
        let length = Int(UInt32(bytes[8]) | UInt32(bytes[9]) << 8 | UInt32(bytes[10]) << 16 | UInt32(bytes[11]) << 24)
        guard length > 0, length <= maximumPayload else { throw DecodeError.malformed }
        return length
    }

    /// Decodes one complete request frame (header and payload).
    public static func decodeRequest(from frame: Data) throws -> Request {
        guard frame.count >= headerSize else { throw DecodeError.malformed }
        let header = frame.prefix(headerSize)
        let length = try payloadLength(inRequestHeader: Data(header))
        guard frame.count == headerSize + length,
              let kind = Kind(rawValue: frame[frame.startIndex + 5]) else { throw DecodeError.malformed }
        let payload = frame.suffix(length)
        // Control characters and NUL cannot belong to a path or URL a user meant
        // to open; a newline in particular must not reach a log or a command.
        guard !payload.contains(where: { $0 < 0x20 || $0 == 0x7f }),
              let value = String(data: payload, encoding: .utf8) else { throw DecodeError.malformed }
        return Request(kind: kind, value: value)
    }

    public static func encode(_ request: Request) throws -> Data {
        let payload = Data(request.value.utf8)
        guard !payload.isEmpty, payload.count <= maximumPayload else { throw DecodeError.malformed }
        return frame(magic: "NPOP", code: request.kind.rawValue, payload: payload)
    }

    /// The message is cut to the limit on a character boundary.
    public static func encode(_ response: Response) -> Data {
        var message = response.message
        while message.utf8.count > maximumMessage { message.removeLast() }
        return frame(magic: "NPOR", code: response.status.rawValue, payload: Data(message.utf8))
    }

    public static func decodeResponse(from frame: Data) throws -> Response {
        guard frame.count >= headerSize else { throw DecodeError.malformed }
        let bytes = [UInt8](frame.prefix(headerSize))
        let length = Int(UInt32(bytes[8]) | UInt32(bytes[9]) << 8 | UInt32(bytes[10]) << 16 | UInt32(bytes[11]) << 24)
        guard Array(bytes[0..<4]) == Array("NPOR".utf8), bytes[4] == version, bytes[6] == 0, bytes[7] == 0,
              let status = Status(rawValue: bytes[5]), length <= maximumMessage,
              frame.count == headerSize + length,
              let message = String(data: frame.suffix(length), encoding: .utf8) else { throw DecodeError.malformed }
        return Response(status: status, message: message)
    }

    private static func frame(magic: String, code: UInt8, payload: Data) -> Data {
        var data = Data(magic.utf8)
        data.append(version)
        data.append(code)
        data.append(contentsOf: [0, 0])
        var length = UInt32(payload.count).littleEndian
        withUnsafeBytes(of: &length) { data.append(contentsOf: $0) }
        data.append(payload)
        return data
    }
}
