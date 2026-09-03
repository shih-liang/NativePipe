import Foundation

/// The virtio-gpu wire protocol, as much of it as NativePipe speaks.
///
/// NativePipe implements VirGL and Venus render nodes and advertises zero
/// scanouts. Application windows use renderer resources; Apple's separate
/// Virtio graphics device owns the optional full-VM framebuffer.
public enum VirtioGPU {

    // MARK: - Device identity

    /// virtio device ID 16 is GPU. The PCI class is the standard display
    /// controller pair; the guest's `virtio_gpu` driver binds on the virtio ID,
    /// not on the PCI class, but lspci output is much less confusing with it set.
    public static let deviceID: UInt16 = 16
    public static let pciClassDisplay: UInt8 = 0x03
    public static let pciSubclassOther: UInt8 = 0x80

    /// Queue 0 carries every command below; queue 1 carries only cursor updates.
    /// The split exists so a cursor move never queues behind a frame.
    public static let controlQueueIndex: UInt16 = 0
    public static let cursorQueueIndex: UInt16 = 1
    public static let queueCount: UInt16 = 2
    public static let maximumScanouts = 16

    // MARK: - Feature bits

    public enum Feature {
        /// Despite the name this bit does not commit the device to the virgl
        /// protocol — the renderer is chosen by the capset in `CTX_CREATE`. To
        /// Linux it means "this device has 3D capability", and it is what sets
        /// `has_virgl_3d`. Without it `DRM_IOCTL_VIRTGPU_CONTEXT_INIT` and
        /// HOST3D blob creation both fail with `EINVAL`, so venus cannot start.
        public static let virgl: UInt32 = 0
        public static let edid: UInt32 = 1
        public static let resourceUUID: UInt32 = 2
        /// Blob resources — the whole point. Without this there is no
        /// `RESOURCE_CREATE_BLOB` and therefore no host-visible memory.
        public static let resourceBlob: UInt32 = 3
        /// Lets the guest create a context bound to a specific capset, which is
        /// how it asks for venus rather than virgl.
        public static let contextInit: UInt32 = 4

        public static func mask(_ bits: [UInt32]) -> UInt32 {
            bits.reduce(0) { $0 | (1 << $1) }
        }
    }

    // MARK: - Capability sets

    /// Capset IDs as understood by Mesa and crosvm. NativePipe offers VirGL,
    /// VirGL2, and Venus from the same renderer instance.
    public enum Capset: UInt32 {
        case virgl = 1
        case virgl2 = 2
        case gfxstream = 3
        case venus = 4
        case crossDomain = 5
    }

    // MARK: - Shared memory regions

    /// `VIRTIO_GPU_SHM_ID_HOST_VISIBLE`. Region 0 is `SHM_ID_UNDEFINED`, so a
    /// region advertised under it is invisible to the guest driver even though
    /// the PCI capability is present and correct — the symptom is a working
    /// device that reports `-host_visible`.
    ///
    /// `RESOURCE_MAP_BLOB` offsets are offsets into this region.
    public static let hostVisibleRegionID: UInt8 = 1

    // MARK: - Commands

    public enum CommandType: UInt32 {
        // 2D / scanout
        case getDisplayInfo = 0x0100
        case resourceCreate2D = 0x0101
        case resourceUnref = 0x0102
        case setScanout = 0x0103
        case resourceFlush = 0x0104
        case transferToHost2D = 0x0105
        case resourceAttachBacking = 0x0106
        case resourceDetachBacking = 0x0107
        case getCapsetInfo = 0x0108
        case getCapset = 0x0109
        case getEDID = 0x010a
        case resourceAssignUUID = 0x010b
        case resourceCreateBlob = 0x010c
        case setScanoutBlob = 0x010d

        // 3D / context
        case ctxCreate = 0x0200
        case ctxDestroy = 0x0201
        case ctxAttachResource = 0x0202
        case ctxDetachResource = 0x0203
        case resourceCreate3D = 0x0204
        case transferToHost3D = 0x0205
        case transferFromHost3D = 0x0206
        case submit3D = 0x0207
        case resourceMapBlob = 0x0208
        case resourceUnmapBlob = 0x0209

        // Cursor
        case updateCursor = 0x0300
        case moveCursor = 0x0301
    }

    public enum ResponseType: UInt32 {
        case okNoData = 0x1100
        case okDisplayInfo = 0x1101
        case okCapsetInfo = 0x1102
        case okCapset = 0x1103
        case okEDID = 0x1104
        case okResourceUUID = 0x1105
        case okMapInfo = 0x1106

        case errUnspecified = 0x1200
        case errOutOfMemory = 0x1201
        case errInvalidScanoutID = 0x1202
        case errInvalidResourceID = 0x1203
        case errInvalidContextID = 0x1204
        case errInvalidParameter = 0x1205
    }

    public enum HeaderFlag {
        /// The guest wants a fence: the response must not be written until the
        /// work has actually completed on the host GPU.
        public static let fence: UInt32 = 1 << 0
        public static let infoRingIndex: UInt32 = 1 << 1
    }

    // MARK: - Blob parameters

    public enum BlobMemory: UInt32 {
        /// Backed by guest pages listed in the command's mem entries.
        case guest = 0x0001
        /// Backed by host memory, made visible through the shared memory region.
        case host3D = 0x0002
        case host3DGuest = 0x0003
    }

    public enum BlobFlag {
        /// The guest intends to map this resource into its address space, which
        /// is what makes it eligible for the host-visible region.
        public static let useMappable: UInt32 = 1 << 0
        public static let useShareable: UInt32 = 1 << 1
        public static let useCrossDevice: UInt32 = 1 << 2
    }

    /// Caching attributes reported back by `RESOURCE_MAP_BLOB`.
    public enum MapCache: UInt32 {
        case cached = 0x01
        case uncached = 0x02
        case writeCombine = 0x03
    }

    /// `enum virtio_gpu_formats`. Linux's dumb framebuffer path normally uses
    /// one of the first two entries (ARGB/XRGB in little-endian BGRA memory).
    public enum Format: UInt32 {
        case b8g8r8a8Unorm = 1
        case b8g8r8x8Unorm = 2
        case a8r8g8b8Unorm = 3
        case x8r8g8b8Unorm = 4
        case r8g8b8a8Unorm = 67
        case x8b8g8r8Unorm = 68
        case a8b8g8r8Unorm = 121
        case r8g8b8x8Unorm = 134

        public var isSupportedScanout32Bit: Bool {
            self == .b8g8r8a8Unorm || self == .b8g8r8x8Unorm
        }
    }
}

// MARK: - Header

extension VirtioGPU {
    /// `struct virtio_gpu_ctrl_hdr` — 24 bytes, prefixing every command and
    /// every response on both queues.
    public struct ControlHeader: Equatable {
        public static let byteCount = 24

        public var type: UInt32
        public var flags: UInt32
        public var fenceID: UInt64
        public var contextID: UInt32
        public var ringIndex: UInt8

        public init(
            type: UInt32, flags: UInt32 = 0, fenceID: UInt64 = 0,
            contextID: UInt32 = 0, ringIndex: UInt8 = 0
        ) {
            self.type = type
            self.flags = flags
            self.fenceID = fenceID
            self.contextID = contextID
            self.ringIndex = ringIndex
        }

        public var wantsFence: Bool { flags & HeaderFlag.fence != 0 }

        public var command: CommandType? { CommandType(rawValue: type) }

        public init(parsing reader: inout LittleEndianReader) throws {
            type = try reader.readUInt32()
            flags = try reader.readUInt32()
            fenceID = try reader.readUInt64()
            contextID = try reader.readUInt32()
            ringIndex = try reader.readUInt8()
            try reader.skip(3)  // padding
        }

        /// Builds the header of a reply to this command, preserving the fence
        /// identity so the guest can match it.
        public func reply(_ response: ResponseType) -> ControlHeader {
            ControlHeader(
                type: response.rawValue,
                flags: flags & HeaderFlag.fence,
                fenceID: fenceID,
                contextID: contextID,
                ringIndex: ringIndex)
        }

        public func encoded() -> Data {
            var writer = LittleEndianWriter()
            writer.write(type)
            writer.write(flags)
            writer.write(fenceID)
            writer.write(contextID)
            writer.write(ringIndex)
            writer.pad(3)
            return writer.data
        }
    }
}

// MARK: - Device configuration space

extension VirtioGPU {
    /// `struct virtio_gpu_config`, the device's 16-byte configuration space.
    /// A render-only instance reports zero scanouts; the framebuffer-enabled
    /// instance reports one.
    public struct DeviceConfig {
        public var eventsRead: UInt32 = 0
        public var eventsClear: UInt32 = 0
        public var numScanouts: UInt32 = 0
        public var numCapsets: UInt32

        public init(numScanouts: UInt32 = 0, numCapsets: UInt32) {
            self.numScanouts = numScanouts
            self.numCapsets = numCapsets
        }

        public func encoded() -> Data {
            var writer = LittleEndianWriter()
            writer.write(eventsRead)
            writer.write(eventsClear)
            writer.write(numScanouts)
            writer.write(numCapsets)
            return writer.data
        }
    }
}

// MARK: - Command bodies

extension VirtioGPU {
    public struct Rect: Equatable, Sendable {
        public var x: UInt32
        public var y: UInt32
        public var width: UInt32
        public var height: UInt32

        public init(x: UInt32, y: UInt32, width: UInt32, height: UInt32) {
            self.x = x
            self.y = y
            self.width = width
            self.height = height
        }

        public init(parsing reader: inout LittleEndianReader) throws {
            x = try reader.readUInt32()
            y = try reader.readUInt32()
            width = try reader.readUInt32()
            height = try reader.readUInt32()
        }

        public func encoded(into writer: inout LittleEndianWriter) {
            writer.write(x)
            writer.write(y)
            writer.write(width)
            writer.write(height)
        }

        public func fits(width resourceWidth: UInt32, height resourceHeight: UInt32) -> Bool {
            guard width > 0, height > 0, x <= resourceWidth, y <= resourceHeight else {
                return false
            }
            let (right, xOverflow) = x.addingReportingOverflow(width)
            let (bottom, yOverflow) = y.addingReportingOverflow(height)
            return !xOverflow && !yOverflow && right <= resourceWidth && bottom <= resourceHeight
        }
    }

    public struct DisplayMode: Equatable, Sendable {
        public var rectangle: Rect
        public var enabled: UInt32
        public var flags: UInt32

        public init(rectangle: Rect = Rect(x: 0, y: 0, width: 0, height: 0),
                    enabled: Bool = false, flags: UInt32 = 0) {
            self.rectangle = rectangle
            self.enabled = enabled ? 1 : 0
            self.flags = flags
        }

        fileprivate func encoded(into writer: inout LittleEndianWriter) {
            rectangle.encoded(into: &writer)
            writer.write(enabled)
            writer.write(flags)
        }
    }

    public struct DisplayInfoResponse {
        public var modes: [DisplayMode]

        public init(width: UInt32, height: UInt32, enabled: Bool) {
            let primary = DisplayMode(
                rectangle: Rect(x: 0, y: 0, width: width, height: height),
                enabled: enabled)
            modes = [primary]
            modes.append(contentsOf: repeatElement(DisplayMode(), count: maximumScanouts - 1))
        }

        public func encoded() -> Data {
            var writer = LittleEndianWriter()
            for mode in modes.prefix(maximumScanouts) { mode.encoded(into: &writer) }
            if modes.count < maximumScanouts {
                for _ in modes.count..<maximumScanouts { DisplayMode().encoded(into: &writer) }
            }
            return writer.data
        }
    }

    public struct ResourceCreate2D {
        public var resourceID: UInt32
        public var format: UInt32
        public var width: UInt32
        public var height: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            resourceID = try reader.readUInt32()
            format = try reader.readUInt32()
            width = try reader.readUInt32()
            height = try reader.readUInt32()
        }
    }

    public struct SetScanout {
        public var rectangle: Rect
        public var scanoutID: UInt32
        public var resourceID: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            rectangle = try Rect(parsing: &reader)
            scanoutID = try reader.readUInt32()
            resourceID = try reader.readUInt32()
        }
    }

    public struct ResourceFlush {
        public var rectangle: Rect
        public var resourceID: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            rectangle = try Rect(parsing: &reader)
            resourceID = try reader.readUInt32()
            try reader.skip(4)
        }
    }

    public struct TransferToHost2D {
        public var rectangle: Rect
        public var offset: UInt64
        public var resourceID: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            rectangle = try Rect(parsing: &reader)
            offset = try reader.readUInt64()
            resourceID = try reader.readUInt32()
            try reader.skip(4)
        }
    }

    public struct ResourceAttachBacking {
        public var resourceID: UInt32
        public var entries: [MemoryEntry]

        public init(parsing reader: inout LittleEndianReader) throws {
            resourceID = try reader.readUInt32()
            let count = try reader.readUInt32()
            guard count <= UInt32(reader.remaining / 16) else {
                throw LittleEndianReader.Failure.truncated(
                    needed: Int(count) * 16, available: reader.remaining)
            }
            entries = try (0..<count).map { _ in try MemoryEntry(parsing: &reader) }
        }
    }

    public struct ResourceDetachBacking {
        public var resourceID: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            resourceID = try reader.readUInt32()
            try reader.skip(4)
        }
    }

    /// `struct virtio_gpu_resource_create_3d`.
    public struct ResourceCreate3D {
        public var resourceID: UInt32
        public var target: UInt32
        public var format: UInt32
        public var bind: UInt32
        public var width: UInt32
        public var height: UInt32
        public var depth: UInt32
        public var arraySize: UInt32
        public var lastLevel: UInt32
        public var sampleCount: UInt32
        public var flags: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            resourceID = try reader.readUInt32()
            target = try reader.readUInt32()
            format = try reader.readUInt32()
            bind = try reader.readUInt32()
            width = try reader.readUInt32()
            height = try reader.readUInt32()
            depth = try reader.readUInt32()
            arraySize = try reader.readUInt32()
            lastLevel = try reader.readUInt32()
            sampleCount = try reader.readUInt32()
            flags = try reader.readUInt32()
            try reader.skip(4)
        }
    }

    public struct Box3D {
        public var x: UInt32
        public var y: UInt32
        public var z: UInt32
        public var width: UInt32
        public var height: UInt32
        public var depth: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            x = try reader.readUInt32()
            y = try reader.readUInt32()
            z = try reader.readUInt32()
            width = try reader.readUInt32()
            height = try reader.readUInt32()
            depth = try reader.readUInt32()
        }
    }

    /// Shared body of TRANSFER_TO_HOST_3D and TRANSFER_FROM_HOST_3D.
    public struct Transfer3D {
        public var box: Box3D
        public var offset: UInt64
        public var resourceID: UInt32
        public var level: UInt32
        public var stride: UInt32
        public var layerStride: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            box = try Box3D(parsing: &reader)
            offset = try reader.readUInt64()
            resourceID = try reader.readUInt32()
            level = try reader.readUInt32()
            stride = try reader.readUInt32()
            layerStride = try reader.readUInt32()
        }
    }

    public struct SetScanoutBlob {
        public var rectangle: Rect
        public var scanoutID: UInt32
        public var resourceID: UInt32
        public var width: UInt32
        public var height: UInt32
        public var format: UInt32
        public var strides: [UInt32]
        public var offsets: [UInt32]

        public init(parsing reader: inout LittleEndianReader) throws {
            rectangle = try Rect(parsing: &reader)
            scanoutID = try reader.readUInt32()
            resourceID = try reader.readUInt32()
            width = try reader.readUInt32()
            height = try reader.readUInt32()
            format = try reader.readUInt32()
            try reader.skip(4)
            strides = try (0..<4).map { _ in try reader.readUInt32() }
            offsets = try (0..<4).map { _ in try reader.readUInt32() }
        }
    }

    public struct GetCapsetInfo {
        public var capsetIndex: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            capsetIndex = try reader.readUInt32()
            try reader.skip(4)
        }
    }

    public struct CapsetInfoResponse {
        public var capsetID: UInt32
        public var capsetMaxVersion: UInt32
        public var capsetMaxSize: UInt32

        public init(capsetID: UInt32, capsetMaxVersion: UInt32, capsetMaxSize: UInt32) {
            self.capsetID = capsetID
            self.capsetMaxVersion = capsetMaxVersion
            self.capsetMaxSize = capsetMaxSize
        }

        public func encoded() -> Data {
            var writer = LittleEndianWriter()
            writer.write(capsetID)
            writer.write(capsetMaxVersion)
            writer.write(capsetMaxSize)
            writer.pad(4)
            return writer.data
        }
    }

    public struct GetCapset {
        public var capsetID: UInt32
        public var capsetVersion: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            capsetID = try reader.readUInt32()
            capsetVersion = try reader.readUInt32()
        }
    }

    public struct ContextCreate {
        public var nameLength: UInt32
        /// The low byte selects the capset — this is how the guest asks for venus.
        public var contextInit: UInt32
        public var debugName: String

        public static let capsetIDMask: UInt32 = 0xff

        public var capsetID: UInt32 { contextInit & Self.capsetIDMask }

        public init(parsing reader: inout LittleEndianReader) throws {
            nameLength = try reader.readUInt32()
            contextInit = try reader.readUInt32()
            let raw = try reader.readBytes(64)
            let clamped = raw.prefix(Int(min(nameLength, 64)))
            debugName = String(decoding: clamped, as: UTF8.self)
        }
    }

    public struct ResourceCreateBlob {
        public var resourceID: UInt32
        public var blobMemory: UInt32
        public var blobFlags: UInt32
        public var entryCount: UInt32
        public var blobID: UInt64
        public var size: UInt64
        /// Guest physical ranges backing the resource, present for guest-memory
        /// blobs. Host-memory blobs carry none.
        public var entries: [MemoryEntry]

        public init(parsing reader: inout LittleEndianReader) throws {
            resourceID = try reader.readUInt32()
            blobMemory = try reader.readUInt32()
            blobFlags = try reader.readUInt32()
            entryCount = try reader.readUInt32()
            blobID = try reader.readUInt64()
            size = try reader.readUInt64()
            entries = try (0..<entryCount).map { _ in try MemoryEntry(parsing: &reader) }
        }
    }

    /// `struct virtio_gpu_mem_entry` — a guest physical range. The host reaches
    /// it with `VZCustomVirtioDevice.guestMemoryMapping(atPhysicalAddress:length:)`.
    public struct MemoryEntry {
        public var address: UInt64
        public var length: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            address = try reader.readUInt64()
            length = try reader.readUInt32()
            try reader.skip(4)
        }
    }

    public struct ResourceMapBlob {
        public var resourceID: UInt32
        /// Where in the host-visible shared memory region the guest wants this
        /// resource to appear. Passed straight to `mapMemory(atOffset:)`.
        public var offset: UInt64

        public init(parsing reader: inout LittleEndianReader) throws {
            resourceID = try reader.readUInt32()
            try reader.skip(4)
            offset = try reader.readUInt64()
        }
    }

    public struct MapInfoResponse {
        public var mapInfo: UInt32

        public init(mapInfo: MapCache) {
            self.mapInfo = mapInfo.rawValue
        }

        public init(mapInfo: UInt32) {
            self.mapInfo = mapInfo
        }

        public func encoded() -> Data {
            var writer = LittleEndianWriter()
            writer.write(mapInfo)
            writer.pad(4)
            return writer.data
        }
    }

    public struct ResourceUnmapBlob {
        public var resourceID: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            resourceID = try reader.readUInt32()
            try reader.skip(4)
        }
    }

    public struct ResourceUnref {
        public var resourceID: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            resourceID = try reader.readUInt32()
            try reader.skip(4)
        }
    }

    public struct ContextResource {
        public var resourceID: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            resourceID = try reader.readUInt32()
            try reader.skip(4)
        }
    }

    /// `SUBMIT_3D`. Only the length is ours; the payload is venus protocol and
    /// is handed over without inspection.
    public struct Submit3D {
        public var payloadByteCount: UInt32

        public init(parsing reader: inout LittleEndianReader) throws {
            payloadByteCount = try reader.readUInt32()
            try reader.skip(4)
        }
    }
}
