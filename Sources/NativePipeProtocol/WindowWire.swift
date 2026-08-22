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
    public static let sceneMagic: [UInt8] = Array("NPSN".utf8)
    public static let sceneVersion: UInt16 = 1
    public static let sceneHeaderSize = 56
    public static let sceneLayerSize = 88
    public static let maximumSceneLayers = 128

    public enum DecodeError: Error, Equatable {
        case notBinaryScene
        case truncated
        case unsupportedVersion(UInt16)
        case malformed
    }

    /// Decode one guest-to-host scene snapshot. Every count and offset is
    /// validated before a value is read; callers can safely reject malformed or
    /// newer messages without losing NPIP stream framing.
    public static func guestEvent(from payload: Data) throws -> Windowing.GuestEvent {
        guard payload.count >= 4,
              Array(payload.prefix(4)) == sceneMagic else {
            throw DecodeError.notBinaryScene
        }
        var reader = Reader(payload)
        try reader.skip(4)
        let version: UInt16 = try reader.integer()
        guard version == sceneVersion else { throw DecodeError.unsupportedVersion(version) }
        let headerSize = Int(try reader.integer() as UInt16)
        let totalSize = Int(try reader.integer() as UInt32)
        guard headerSize == sceneHeaderSize, totalSize == payload.count,
              totalSize >= sceneHeaderSize else { throw DecodeError.malformed }

        let surface: UInt32 = try reader.integer()
        let presentationID: UInt32 = try reader.integer()
        let width = Int(try reader.integer() as UInt32)
        let height = Int(try reader.integer() as UInt32)
        let scale = Int(try reader.integer() as UInt32)
        let geometry = Windowing.Rect(
            x: Int(try reader.integer() as Int32),
            y: Int(try reader.integer() as Int32),
            width: Int(try reader.integer() as Int32),
            height: Int(try reader.integer() as Int32))
        let layerCount = Int(try reader.integer() as UInt32)
        _ = try reader.integer() as UInt32 // header flags; none in version 1

        guard surface != 0, presentationID != 0,
              width > 0, height > 0, scale >= 1, scale <= 4,
              geometry.width > 0, geometry.height > 0,
              layerCount > 0, layerCount <= maximumSceneLayers,
              layerCount <= (Int.max - sceneHeaderSize) / sceneLayerSize,
              sceneHeaderSize + layerCount * sceneLayerSize == totalSize
        else { throw DecodeError.malformed }

        var layers: [Windowing.SceneLayer] = []
        layers.reserveCapacity(layerCount)
        for _ in 0..<layerCount {
            let layerSurface: UInt32 = try reader.integer()
            let resourceID: UInt32 = try reader.integer()
            let layerWidth = Int(try reader.integer() as UInt32)
            let layerHeight = Int(try reader.integer() as UInt32)
            let bytesPerRow = Int(try reader.integer() as UInt32)
            let formatRaw: UInt16 = try reader.integer()
            let flags: UInt16 = try reader.integer()
            let transformRaw: UInt32 = try reader.integer()
            _ = try reader.integer() as UInt32
            let destination = try reader.floatRect()
            let source = try reader.floatRect()
            let clip = try reader.floatRect()
            let alpha = try reader.float()
            _ = try reader.integer() as UInt32

            let format: Windowing.PixelFormat
            switch formatRaw {
            case 1: format = .bgra8888
            case 2: format = .bgrx8888
            case 3: format = .rgba8888
            default: throw DecodeError.malformed
            }
            guard let transform = Windowing.BufferTransform(rawValue: transformRaw),
                  layerSurface != 0, resourceID != 0,
                  layerWidth > 0, layerHeight > 0,
                  bytesPerRow >= layerWidth * 4,
                  alpha.isFinite, alpha >= 0, alpha <= 1,
                  destination.isValidPositive, source.isValidPositive,
                  clip.isValidPositive,
                  source.x >= 0, source.y >= 0,
                  source.x + source.width <= Double(layerWidth) + 0.01,
                  source.y + source.height <= Double(layerHeight) + 0.01
            else { throw DecodeError.malformed }

            layers.append(Windowing.SceneLayer(
                surface: layerSurface, resourceID: resourceID,
                width: layerWidth, height: layerHeight,
                bytesPerRow: bytesPerRow, format: format,
                destination: destination, sourcePixels: source, clip: clip,
                alpha: alpha, opaque: flags & 1 != 0, transform: transform))
        }
        guard reader.isAtEnd else { throw DecodeError.malformed }
        return .sceneCommitted(scene: Windowing.SceneSnapshot(
            surface: surface, presentationID: presentationID,
            width: width, height: height, scale: scale,
            windowGeometry: geometry, layers: layers))
    }

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

    private struct Reader {
        let data: Data
        var offset = 0

        init(_ data: Data) { self.data = data }
        var isAtEnd: Bool { offset == data.count }

        mutating func skip(_ count: Int) throws {
            guard count >= 0, offset <= data.count - count else { throw DecodeError.truncated }
            offset += count
        }

        mutating func integer<T: FixedWidthInteger>() throws -> T {
            let size = MemoryLayout<T>.size
            guard offset <= data.count - size else { throw DecodeError.truncated }
            var value: T = 0
            _ = withUnsafeMutableBytes(of: &value) { destination in
                data.copyBytes(to: destination, from: offset..<(offset + size))
            }
            offset += size
            return T(littleEndian: value)
        }

        mutating func float() throws -> Float {
            Float(bitPattern: try integer() as UInt32)
        }

        mutating func floatRect() throws -> Windowing.FloatRect {
            Windowing.FloatRect(
                x: Double(try float()), y: Double(try float()),
                width: Double(try float()), height: Double(try float()))
        }
    }
}

private extension Windowing.FloatRect {
    var isValidPositive: Bool {
        x.isFinite && y.isFinite && width.isFinite && height.isFinite &&
        width > 0 && height > 0
    }
}
