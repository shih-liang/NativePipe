import Foundation

/// Allocation-free binary payloads for high-rate window-channel messages.
///
/// The outer NPIP frame remains unchanged so old JSON control messages and the
/// binary fast path can share one stream. Integer fields are explicitly little
/// endian; this is a wire format, never a Swift struct memory dump.
public enum WindowWire {
    public static let motionMagic: [UInt8] = Array("NPMO".utf8)
    public static let motionPayloadSize = 16
    public static let scrollMagic: [UInt8] = Array("NPSC".utf8)

    /// Encodes the only replaceable high-rate host command. Other commands stay
    /// JSON for now because their ordering matters and their rate is negligible.
    public static func fastPayload(for command: Windowing.HostCommand) -> Data? {
        switch command {
        case .pointerMoved(let window, let x, let y):
            var payload = Data(motionMagic)
            append(window, to: &payload)
            append(fixed24_8(x), to: &payload)
            append(fixed24_8(y), to: &payload)
            return payload
        case .pointerScroll(let window, let dx, let dy, let precise):
            var payload = Data(scrollMagic)
            append(window, to: &payload)
            append(dx.bitPattern, to: &payload)
            append(dy.bitPattern, to: &payload)
            payload.append(precise ? 1 : 0)
            payload.append(contentsOf: [0, 0, 0])
            return payload
        default:
            return nil
        }
    }

    private static func fixed24_8(_ value: Double) -> Int32 {
        let scaled = (value * 256).rounded()
        if scaled <= Double(Int32.min) { return .min }
        if scaled >= Double(Int32.max) { return .max }
        return Int32(scaled)
    }

    private static func append<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var little = value.littleEndian
        Swift.withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
    }
}
