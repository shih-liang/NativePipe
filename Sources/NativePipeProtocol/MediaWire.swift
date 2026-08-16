import Foundation

/// Encoded media frame on the NativePipe media port (`NativePipePort.media`).
///
/// Layout is little-endian, fixed 32-byte header, then `payloadLength` bytes
/// of codec bitstream (H.264 Annex-B for codec == h264).
public enum MediaWire {
    public static let headerSize = 32
    public static let maximumPayloadSize = 32 * 1024 * 1024
    public static let magic = Data("NPEN".utf8)

    public enum Codec: UInt8, Sendable {
        case h264 = 1
    }

    public struct Header: Sendable {
        public var version: UInt8
        public var codec: Codec
        public var flags: UInt8
        public var surfaceID: UInt32
        public var width: UInt16
        public var height: UInt16
        public var ptsNanos: UInt64
        public var payloadLength: UInt32
        public var bitstreamEpoch: UInt16

        public init(
            version: UInt8 = 1,
            codec: Codec = .h264,
            flags: UInt8 = 0,
            surfaceID: UInt32,
            width: UInt16,
            height: UInt16,
            ptsNanos: UInt64,
            payloadLength: UInt32,
            bitstreamEpoch: UInt16 = 0
        ) {
            self.version = version
            self.codec = codec
            self.flags = flags
            self.surfaceID = surfaceID
            self.width = width
            self.height = height
            self.ptsNanos = ptsNanos
            self.payloadLength = payloadLength
            self.bitstreamEpoch = bitstreamEpoch
        }

        public func encoded() -> Data {
            var data = Data(capacity: MediaWire.headerSize)
            data.append(MediaWire.magic)
            data.append(version)
            data.append(codec.rawValue)
            data.append(flags)
            data.append(0) // pad to align surfaceID
            appendUInt32(&data, surfaceID)
            appendUInt16(&data, width)
            appendUInt16(&data, height)
            appendUInt64(&data, ptsNanos)
            appendUInt32(&data, payloadLength)
            appendUInt16(&data, bitstreamEpoch)
            appendUInt16(&data, 0) // reserved
            return data
        }

        public static func parse(from data: Data) -> Header? {
            guard data.count >= MediaWire.headerSize else { return nil }
            guard data.prefix(4).elementsEqual(MediaWire.magic) else { return nil }
            let version = data[4]
            guard version == 1 else { return nil }
            guard let codec = Codec(rawValue: data[5]) else { return nil }
            let flags = data[6]
            let surfaceID = readUInt32(data, 8)
            let width = readUInt16(data, 12)
            let height = readUInt16(data, 14)
            let pts = readUInt64(data, 16)
            let length = readUInt32(data, 24)
            let epoch = readUInt16(data, 28)
            return Header(
                version: version, codec: codec, flags: flags,
                surfaceID: surfaceID, width: width, height: height,
                ptsNanos: pts, payloadLength: length, bitstreamEpoch: epoch)
        }
    }

    /// Incremental demultiplexer for a TCP byte stream of NPEN frames.
    public final class Demuxer: @unchecked Sendable {
        private var buffer = Data()

        public init() {}

        public func push(_ chunk: Data) -> [(Header, Data)] {
            buffer.append(chunk)
            var out: [(Header, Data)] = []
            var offset = 0
            while buffer.count - offset >= MediaWire.headerSize {
                let headerBytes = buffer.subdata(in: offset..<(offset + MediaWire.headerSize))
                guard let header = Header.parse(from: headerBytes),
                      header.payloadLength <= UInt32(MediaWire.maximumPayloadSize) else {
                    // Resync without repeatedly moving the complete receive buffer.
                    offset += 1
                    continue
                }
                let total = MediaWire.headerSize + Int(header.payloadLength)
                guard buffer.count - offset >= total else { break }
                let payloadStart = offset + MediaWire.headerSize
                let payload = buffer.subdata(in: payloadStart..<(offset + total))
                offset += total
                out.append((header, payload))
            }
            if offset > 0 { buffer.removeSubrange(0..<offset) }
            // A corrupt stream without a complete header must not grow forever.
            if buffer.count > MediaWire.maximumPayloadSize + MediaWire.headerSize {
                buffer.removeAll(keepingCapacity: true)
            }
            return out
        }
    }
}

private func appendUInt16(_ data: inout Data, _ value: UInt16) {
    var le = value.littleEndian
    withUnsafeBytes(of: &le) { data.append(contentsOf: $0) }
}

private func appendUInt32(_ data: inout Data, _ value: UInt32) {
    var le = value.littleEndian
    withUnsafeBytes(of: &le) { data.append(contentsOf: $0) }
}

private func appendUInt64(_ data: inout Data, _ value: UInt64) {
    var le = value.littleEndian
    withUnsafeBytes(of: &le) { data.append(contentsOf: $0) }
}

private func readUInt16(_ data: Data, _ offset: Int) -> UInt16 {
    data.withUnsafeBytes { raw in
        UInt16(littleEndian: raw.loadUnaligned(fromByteOffset: offset, as: UInt16.self))
    }
}

private func readUInt32(_ data: Data, _ offset: Int) -> UInt32 {
    data.withUnsafeBytes { raw in
        UInt32(littleEndian: raw.loadUnaligned(fromByteOffset: offset, as: UInt32.self))
    }
}

private func readUInt64(_ data: Data, _ offset: Int) -> UInt64 {
    data.withUnsafeBytes { raw in
        UInt64(littleEndian: raw.loadUnaligned(fromByteOffset: offset, as: UInt64.self))
    }
}
