import Foundation

/// Length-prefixed framing for the host <-> guestd control channel.
///
/// Layout of one frame:
///
///     0      4       5        8            12
///     +------+-------+--------+------------+============+
///     | NPIP | ver=1 | rsvd   | length(LE) |  payload   |
///     +------+-------+--------+------------+============+
///
/// The payload is opaque bytes. On the control channel it is `ControlWire`
/// binary; on the window channel it may still be JSON or `WindowWire` binary.
public enum WireFormat {
    public static let magic: [UInt8] = Array("NPIP".utf8)
    public static let version: UInt8 = 1
    public static let headerSize = 12

    /// Refuse absurd frames rather than letting a confused guest allocate the host to death.
    public static let maxPayloadSize = 8 * 1024 * 1024

    public enum Error: Swift.Error, CustomStringConvertible {
        case badMagic([UInt8])
        case unsupportedVersion(UInt8)
        case payloadTooLarge(Int)

        public var description: String {
            switch self {
            case .badMagic(let bytes):
                return "bad frame magic \(bytes)"
            case .unsupportedVersion(let v):
                return "unsupported wire version \(v)"
            case .payloadTooLarge(let n):
                return "payload of \(n) bytes exceeds \(WireFormat.maxPayloadSize)"
            }
        }
    }

    public static func encodeHeader(payloadCount: Int) throws -> Data {
        guard payloadCount <= maxPayloadSize else {
            throw Error.payloadTooLarge(payloadCount)
        }
        var header = Data(magic)
        header.append(version)
        header.append(contentsOf: [0, 0, 0])
        var length = UInt32(payloadCount).littleEndian
        withUnsafeBytes(of: &length) { header.append(contentsOf: $0) }
        return header
    }

    /// Parses a `headerSize`-byte header and returns the payload length that follows.
    public static func decodeHeader(_ header: Data) throws -> Int {
        precondition(header.count == headerSize, "decodeHeader expects exactly \(headerSize) bytes")
        let bytes = [UInt8](header)
        guard Array(bytes[0..<4]) == magic else {
            throw Error.badMagic(Array(bytes[0..<4]))
        }
        guard bytes[4] == version else {
            throw Error.unsupportedVersion(bytes[4])
        }
        let length = bytes[8..<12].reversed().reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        guard length <= UInt32(maxPayloadSize) else {
            throw Error.payloadTooLarge(Int(length))
        }
        return Int(length)
    }

    public static func frame(payload: Data) throws -> Data {
        var out = try encodeHeader(payloadCount: payload.count)
        out.append(payload)
        return out
    }
}

/// Incremental frame reassembler for a byte stream that arrives in arbitrary chunks.
public struct FrameDecoder {
    private var buffer = Data()

    public init() {}

    public mutating func append(_ bytes: Data) {
        buffer.append(bytes)
    }

    /// Pulls the next complete payload out of the buffer, or nil if more bytes are needed.
    public mutating func next() throws -> Data? {
        guard buffer.count >= WireFormat.headerSize else { return nil }
        let header = buffer.prefix(WireFormat.headerSize)
        let payloadLength = try WireFormat.decodeHeader(Data(header))
        let total = WireFormat.headerSize + payloadLength
        guard buffer.count >= total else { return nil }
        let payload = buffer.dropFirst(WireFormat.headerSize).prefix(payloadLength)
        buffer.removeFirst(total)
        return Data(payload)
    }
}
