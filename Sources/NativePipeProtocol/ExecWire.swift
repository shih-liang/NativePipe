import Foundation

/// Framing used after an interactive exec session has connected.
///
/// PTY bytes and control messages must not share an unescaped byte stream:
/// stream sockets may split writes anywhere and user input may contain every
/// possible magic sequence. Each record is therefore independently framed.
public enum ExecWire {
    public static let magic = Data("NPXT".utf8)
    public static let version: UInt8 = 1
    public static let headerSize = 12
    public static let maxPayloadSize = 1 << 20

    public enum Kind: UInt8, Sendable {
        case data = 1
        case resize = 2
        /// Four-byte little-endian process exit status. Older peers may send
        /// an empty payload, which clients interpret as success.
        case exit = 3
        /// The client has no more terminal input. This is directional: the
        /// guest keeps the session open for output and the final exit record.
        case endInput = 4
        /// Non-PTY stderr; never mix diagnostics into binary stdout.
        case stderr = 5
    }

    public struct Record: Sendable {
        public let kind: Kind
        public let payload: Data
    }

    public static func frame(kind: Kind, payload: Data) throws -> Data {
        guard payload.count <= maxPayloadSize else { throw WireFormat.Error.payloadTooLarge(payload.count) }
        var out = magic
        out.append(version)
        out.append(kind.rawValue)
        out.append(contentsOf: [0, 0])
        var length = UInt32(payload.count).littleEndian
        withUnsafeBytes(of: &length) { out.append(contentsOf: $0) }
        out.append(payload)
        return out
    }

    public static func data(_ payload: Data) throws -> Data {
        try frame(kind: .data, payload: payload)
    }

    public static func resize(cols: Int, rows: Int) throws -> Data {
        var payload = Data()
        var c = UInt32(clamping: cols).littleEndian
        var r = UInt32(clamping: rows).littleEndian
        withUnsafeBytes(of: &c) { payload.append(contentsOf: $0) }
        withUnsafeBytes(of: &r) { payload.append(contentsOf: $0) }
        return try frame(kind: .resize, payload: payload)
    }

    public static func endInput() throws -> Data {
        try frame(kind: .endInput, payload: Data())
    }

    public struct Decoder: Sendable {
        private var buffer = Data()

        public init() {}

        public mutating func append(_ data: Data) { buffer.append(data) }

        public mutating func next() throws -> Record? {
            guard buffer.count >= headerSize else { return nil }
            guard buffer.prefix(4) == magic, buffer[4] == version,
                  let kind = Kind(rawValue: buffer[5]) else {
                throw WireFormat.Error.badMagic(Array(buffer.prefix(4)))
            }
            let length = buffer.withUnsafeBytes {
                Int(UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: 8, as: UInt32.self)))
            }
            guard length <= maxPayloadSize else { throw WireFormat.Error.payloadTooLarge(length) }
            let total = headerSize + length
            guard buffer.count >= total else { return nil }
            let payload = Data(buffer[headerSize..<total])
            buffer.removeSubrange(0..<total)
            return Record(kind: kind, payload: payload)
        }
    }
}
