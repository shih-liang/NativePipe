import Foundation

/// Encoded media frame on the NativePipe media port (`NativePipePort.media`).
///
/// Layout is little-endian, fixed 36-byte header, then `payloadLength` bytes
/// of codec bitstream (H.264 Annex-B or AV1 low-overhead OBUs, selected by codec).
public enum MediaWire {
    public static let version: UInt8 = 2
    public static let headerSize = 36
    public static let maximumPayloadSize = 32 * 1024 * 1024
    public static let magic = Data("NPEN".utf8)
    public static let flagHasAlpha: UInt8 = 1 << 0
    public static let flagReuseAlpha: UInt8 = 1 << 1
    /// Set only after the guest's hardware encoder produced this packet.
    public static let flagHardwareEncoded: UInt8 = 1 << 2

    public enum Codec: UInt8, Sendable {
        case h264 = 1
        case alphaRLE = 2
        case av1 = 3
    }

    /// Decodes the PackBits alpha sidecar used for translucent Wayland
    /// surfaces. The exact output size is part of validation, so malformed
    /// network data cannot overrun the destination or silently truncate.
    public static func decodeAlphaRLE(_ payload: Data, pixelCount: Int) -> Data? {
        guard pixelCount > 0, pixelCount <= maximumPayloadSize else { return nil }
        var output = Data(count: pixelCount)
        let valid = output.withUnsafeMutableBytes { destination in
            payload.withUnsafeBytes { source in
                guard let src = source.baseAddress?.assumingMemoryBound(to: UInt8.self),
                      let dst = destination.baseAddress else { return false }
                var input = 0, written = 0
                while input < source.count, written < pixelCount {
                    let tag = src[input]
                    input += 1
                    if tag <= 127 {
                        let count = Int(tag) + 1
                        guard count <= source.count - input, count <= pixelCount - written else { return false }
                        memcpy(dst.advanced(by: written), src.advanced(by: input), count)
                        input += count
                        written += count
                    } else if tag >= 129 {
                        let count = 257 - Int(tag)
                        guard input < source.count, count <= pixelCount - written else { return false }
                        memset(dst.advanced(by: written), Int32(src[input]), count)
                        input += 1
                        written += count
                    } else { return false }
                }
                return input == source.count && written == pixelCount
            }
        }
        return valid ? output : nil
    }

    public struct Header: Sendable {
        public var version: UInt8
        public var codec: Codec
        public var flags: UInt8
        /// Decoder stream. One stream exists per Wayland surface.
        public var surfaceID: UInt32
        /// Immutable decoded frame named by a committed scene layer.
        public var resourceID: UInt32
        public var width: UInt16
        public var height: UInt16
        public var ptsNanos: UInt64
        public var payloadLength: UInt32
        public var bitstreamEpoch: UInt16

        public init(
            version: UInt8 = MediaWire.version,
            codec: Codec = .h264,
            flags: UInt8 = 0,
            surfaceID: UInt32,
            resourceID: UInt32,
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
            self.resourceID = resourceID
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
            appendUInt32(&data, resourceID)
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
            guard version == MediaWire.version else { return nil }
            guard let codec = Codec(rawValue: data[5]) else { return nil }
            let flags = data[6]
            let surfaceID = readUInt32(data, 8)
            let resourceID = readUInt32(data, 12)
            let width = readUInt16(data, 16)
            let height = readUInt16(data, 18)
            let pts = readUInt64(data, 20)
            let length = readUInt32(data, 28)
            let epoch = readUInt16(data, 32)
            guard surfaceID != 0, resourceID != 0, width != 0, height != 0 else {
                return nil
            }
            return Header(
                version: version, codec: codec, flags: flags,
                surfaceID: surfaceID, resourceID: resourceID,
                width: width, height: height,
                ptsNanos: pts, payloadLength: length, bitstreamEpoch: epoch)
        }
    }

    /// Incremental demultiplexer for a byte stream of NPEN frames.
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
