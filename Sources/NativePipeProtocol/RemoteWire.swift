import Foundation

/// SSH remains one ordered stream. Bulk records are interruptible at 16 KiB
/// boundaries; receiver credits bound bytes hidden inside SSH/TCP buffers.
/// Control records never wait for bulk credits. Files use separate SSH/SFTP.
public enum RemoteWire {
    public static let fragmentSize = 16 * 1024
    public static let initialWindowSize = 65536
    public static let fragmentMagic = Data("NPRF".utf8)
    public static let acknowledgementMagic = Data("NPRA".utf8)

    public static func acknowledgement(_ count: Int) -> Data {
        var bytes = acknowledgementMagic
        append(UInt32(count), to: &bytes)
        return bytes
    }

    public static func fragment(_ record: Data, offset: Int, lane: UInt32, count: Int = fragmentSize) -> Data {
        let end = min(record.count, offset + count)
        var bytes = fragmentMagic
        append(lane, to: &bytes)
        append(UInt32(record.count), to: &bytes)
        append(UInt32(offset), to: &bytes)
        bytes.append(record[offset..<end])
        // The fixed fragment bound is strictly below NPIP's payload limit.
        return try! WireFormat.frame(payload: bytes)
    }

    public static func number(_ bytes: Data, at offset: Int) -> UInt32 {
        bytes.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self)) }
    }

    private static func append(_ value: UInt32, to bytes: inout Data) {
        var value = value.littleEndian
        withUnsafeBytes(of: &value) { bytes.append(contentsOf: $0) }
    }

    // Same receiver-paced budget as the remote C writer (flow.h). The wire
    // remains reliable: a smaller budget stops admission, never drops bytes.
    public struct FlowControl {
        private var pending: [(bytes: Int, sent: Double)] = []
        public private(set) var bytes = 0
        public private(set) var window = RemoteWire.initialWindowSize
        private var minimumRTT = 0.0, adjusted = 0.0
        private var waiting = false
        private var slowAtFloor = 0
        public init() {}
        public mutating func allowance(remaining: Int) -> Int {
            let chunk = min(remaining, max(1024, min(RemoteWire.fragmentSize, window / 4)))
            guard pending.count < 128, bytes + chunk <= window else { waiting = true; return 0 }
            return chunk
        }
        public mutating func sent(_ count: Int, now: Double) {
            pending.append((count, now)); bytes += count
        }
        public mutating func acknowledge(_ count: Int, now: Double) -> Bool {
            guard count > 0, count <= bytes else { return false }
            bytes -= count
            var remaining = count, rtt = 0.0
            while remaining > 0 {
                let take = min(remaining, pending[0].bytes)
                remaining -= take; pending[0].bytes -= take
                rtt = now - pending[0].sent
                if pending[0].bytes == 0 { pending.removeFirst() }
            }
            guard rtt > 0 else { return true }
            if minimumRTT == 0 || rtt < minimumRTT { minimumRTT = rtt }
            if now - adjusted >= minimumRTT {
                if rtt - minimumRTT > 0.030 + 0.000001 {
                    window = max(4096, window * 3 / 4)
                    if window == 4096 {
                        slowAtFloor += 1
                        if slowAtFloor >= 8 { minimumRTT = rtt; slowAtFloor = 0 }
                    }
                } else {
                    slowAtFloor = 0
                    if waiting { window = min(1_048_576, window * 2) }
                }
                waiting = false; adjusted = now
            }
            return true
        }
    }

    public struct Reassembler {
        private var records: [UInt32: Data] = [:]
        private var sizes: [UInt32: Int] = [:]
        public init() {}
        public var isEmpty: Bool { records.isEmpty }

        /// Lanes 1 and 2 preserve ordering independently (display / background).
        /// A record is never exposed until all its bytes have arrived.
        public mutating func receive(_ payload: Data, maximumSize: Int) throws -> Data? {
            guard payload.count > 16, payload.count <= 16 + RemoteWire.fragmentSize,
                  payload.prefix(4) == RemoteWire.fragmentMagic else { throw CocoaError(.coderReadCorrupt) }
            let lane = RemoteWire.number(payload, at: 4)
            let total = Int(RemoteWire.number(payload, at: 8))
            let offset = Int(RemoteWire.number(payload, at: 12))
            let count = payload.count - 16
            guard (lane == 1 || lane == 2), total > 0, total <= maximumSize,
                  offset <= total, count <= total - offset else { throw CocoaError(.coderReadCorrupt) }
            if offset == 0 {
                guard records[lane] == nil else { throw CocoaError(.coderReadCorrupt) }
                records[lane] = Data(); sizes[lane] = total
            }
            guard records[lane]?.count == offset, sizes[lane] == total else { throw CocoaError(.coderReadCorrupt) }
            records[lane]?.append(payload.dropFirst(16))
            guard offset + count == total else { return nil }
            sizes.removeValue(forKey: lane)
            return records.removeValue(forKey: lane)
        }
    }
}
