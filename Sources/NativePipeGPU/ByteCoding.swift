import Foundation

/// Little-endian cursor over a byte buffer.
///
/// virtio is little-endian once `VIRTIO_F_VERSION_1` is negotiated, which the
/// framework sets for us unconditionally.
///
/// This reads from a `Data` that the caller has already copied out of the queue
/// element **once**. That is not an implementation detail — the guest can
/// rewrite its own memory at any moment, so parsing directly out of the
/// descriptor, or re-reading a field, is a time-of-check/time-of-use bug.
public struct LittleEndianReader {
    public enum Failure: Error, CustomStringConvertible {
        case truncated(needed: Int, available: Int)

        public var description: String {
            switch self {
            case .truncated(let needed, let available):
                return "command truncated: needed \(needed) more bytes, had \(available)"
            }
        }
    }

    private let bytes: [UInt8]
    private var offset = 0

    public init(_ data: Data) {
        bytes = [UInt8](data)
    }

    public var remaining: Int { bytes.count - offset }

    public mutating func skip(_ count: Int) throws {
        try require(count)
        offset += count
    }

    public mutating func readUInt8() throws -> UInt8 {
        try require(1)
        defer { offset += 1 }
        return bytes[offset]
    }

    public mutating func readUInt32() throws -> UInt32 {
        try require(4)
        defer { offset += 4 }
        return (0..<4).reduce(UInt32(0)) { $0 | (UInt32(bytes[offset + $1]) << (8 * $1)) }
    }

    public mutating func readUInt64() throws -> UInt64 {
        try require(8)
        defer { offset += 8 }
        return (0..<8).reduce(UInt64(0)) { $0 | (UInt64(bytes[offset + $1]) << (8 * $1)) }
    }

    public mutating func readBytes(_ count: Int) throws -> [UInt8] {
        try require(count)
        defer { offset += count }
        return Array(bytes[offset..<(offset + count)])
    }

    private func require(_ count: Int) throws {
        guard count >= 0, remaining >= count else {
            throw Failure.truncated(needed: count, available: remaining)
        }
    }
}

public struct LittleEndianWriter {
    public private(set) var data = Data()

    public init() {}

    public mutating func write(_ value: UInt8) {
        data.append(value)
    }

    public mutating func write(_ value: UInt32) {
        for shift in stride(from: 0, to: 32, by: 8) {
            data.append(UInt8truncating(value >> UInt32(shift)))
        }
    }

    public mutating func write(_ value: UInt64) {
        for shift in stride(from: 0, to: 64, by: 8) {
            data.append(UInt8truncating(value >> UInt64(shift)))
        }
    }

    public mutating func pad(_ count: Int) {
        data.append(contentsOf: repeatElement(UInt8(0), count: count))
    }

    private func UInt8truncating<T: BinaryInteger>(_ value: T) -> UInt8 {
        UInt8(truncatingIfNeeded: value)
    }
}
