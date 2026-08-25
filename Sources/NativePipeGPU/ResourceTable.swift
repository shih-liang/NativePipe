import CoreVideo
import Foundation
import IOSurface
import Metal
import os

/// One guest `resource_id` on the host virtio-gpu.
///
/// HOST3D blobs are Venus allocations. CPU-mappable allocations expose vkr's
/// host pointer through the guest aperture; DEVICE_LOCAL allocations stay in a
/// Metal heap and are accessed through their renderer resource id. Application
/// windows never make these resources into IOSurfaces: NativeWindow samples the
/// original Metal texture while composing directly into a CAMetalDrawable.
public final class GPUResource {
    public let resourceID: UInt32
    /// Set only for the legacy virtio-gpu 2D framebuffer path.
    public let surface: IOSurfaceRef?
    public let byteCount: Int
    public let geometry: VirtioGPU.BlobGeometry?
    /// Present only for RESOURCE_CREATE_2D resources. `sourceBytesPerRow` is
    /// the guest-visible tight stride; `geometry.bytesPerRow` is the IOSurface
    /// stride and may be larger because Apple aligns scanout rows.
    public let twoDimensional: Resource2DMetadata?
    /// Mesa's Venus object id, or the packed geometry word for a window.
    public let blobID: UInt64
    /// Standard VirGL RESOURCE_CREATE_3D resource owned by virglrenderer.
    public let isVirglResource: Bool

    /// Offset inside the host-visible shared memory region, once the guest has
    /// asked for it with `RESOURCE_MAP_BLOB`. Nil while unmapped.
    public internal(set) var mappedOffset: UInt64?

    /// `mappedOffset` is cleared as soon as an unmap is claimed so a second
    /// command cannot submit the same VZ unmap twice. The operation itself is
    /// asynchronous, however, and the backing must remain alive until VZ has
    /// actually removed the aperture mapping.
    internal var unmapInFlight = false
    internal var afterUnmap: [(Bool) -> Void] = []

    private let pointer: UnsafeMutableRawPointer?

    /// CPU mapping into the guest aperture. Nil for DEVICE_LOCAL / MTLHeap blobs.
    public var baseAddress: UnsafeMutableRawPointer? { pointer }

    /// True when this blob is CPU-mappable into the guest aperture.
    public var isHostMappable: Bool { pointer != nil }

    init(
        resourceID: UInt32,
        pointer: UnsafeMutableRawPointer?,
        surface: IOSurfaceRef?,
        byteCount: Int,
        geometry: VirtioGPU.BlobGeometry?,
        twoDimensional: Resource2DMetadata? = nil,
        blobID: UInt64,
        isVirglResource: Bool = false
    ) {
        self.resourceID = resourceID
        self.pointer = pointer
        self.surface = surface
        self.byteCount = byteCount
        self.geometry = geometry
        self.twoDimensional = twoDimensional
        self.blobID = blobID
        self.isVirglResource = isVirglResource
    }

    /// A Metal buffer over the exact pages mapped into the guest aperture.
    /// Used by the renderer/device implementation, never by application-window
    /// presentation.
    public func makeMetalBuffer(using device: MTLDevice) -> MTLBuffer? {
        guard let pointer else { return nil }
        return device.makeBuffer(
            bytesNoCopy: pointer,
            length: byteCount,
            options: .storageModeShared,
            deallocator: nil)
    }

    /// A BGRA texture view over a legacy 2D IOSurface resource.
    public func makeMetalTexture(using device: MTLDevice) -> MTLTexture? {
        guard let geometry, let surface else { return nil }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm,
            width: geometry.width,
            height: geometry.height,
            mipmapped: false)
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderRead, .shaderWrite, .renderTarget]
        return device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
    }
}

public struct Resource2DMetadata: Equatable, Sendable {
    public let format: UInt32
    public let width: Int
    public let height: Int
    public let sourceBytesPerRow: Int

    public init(format: UInt32, width: Int, height: Int, sourceBytesPerRow: Int) {
        self.format = format
        self.width = width
        self.height = height
        self.sourceBytesPerRow = sourceBytesPerRow
    }
}

public enum ResourceAllocationError: LocalizedError {
    case duplicateID(UInt32)
    case unknownID(UInt32)
    case allocationFailed(bytes: Int)
    case emptyBlob
    case blobTooLarge(UInt64)
    case tooSmall(allocated: Int, requested: Int)
    case unalignedPointer
    case invalidDimensions(width: UInt32, height: UInt32)
    case unsupportedFormat(UInt32)

    public var errorDescription: String? {
        switch self {
        case .duplicateID(let id): return "resource \(id) already exists"
        case .unknownID(let id): return "no such resource \(id)"
        case .allocationFailed(let bytes): return "could not allocate \(bytes) bytes"
        case .emptyBlob: return "blob size must be greater than zero"
        case .blobTooLarge(let bytes): return "blob size \(bytes) exceeds host address space"
        case .tooSmall(let allocated, let requested):
            return "allocated \(allocated) bytes for a blob the guest will map \(requested) of"
        case .unalignedPointer: return "host mapping is not a multiple of the host page"
        case .invalidDimensions(let width, let height):
            return "invalid 2D resource dimensions \(width)x\(height)"
        case .unsupportedFormat(let format):
            return "unsupported 2D resource format \(format)"
        }
    }
}

/// Owns every live resource. Confined to the device queue — virtio-gpu commands
/// arrive serialised on it, so no locking is needed or wanted here.
public final class ResourceTable {
    static let log = Logger(subsystem: "com.nativepipe.gpu", category: "resources")

    private var resources: [UInt32: GPUResource] = [:]
    private let pageSize = Int(getpagesize())

    public init() {}

    public var count: Int { resources.count }

    public var identifiers: [UInt32] { Array(resources.keys) }

    public var all: [GPUResource] { Array(resources.values) }

    public subscript(id: UInt32) -> GPUResource? { resources[id] }

    private func alignedByteCount(for size: UInt64) throws -> Int {
        guard size > 0 else { throw ResourceAllocationError.emptyBlob }
        guard size <= UInt64(Int.max - (pageSize - 1)) else {
            throw ResourceAllocationError.blobTooLarge(size)
        }
        return (Int(size) + pageSize - 1) / pageSize * pageSize
    }

    /// Linux KMS dumb framebuffer. Guest pages remain the authoritative
    /// backing; TRANSFER_TO_HOST_2D copies only the requested rectangle into
    /// this IOSurface, which is then presented without another CPU copy.
    @discardableResult
    public func create2D(
        id: UInt32, format: UInt32, width: UInt32, height: UInt32
    ) throws -> GPUResource {
        guard resources[id] == nil else { throw ResourceAllocationError.duplicateID(id) }
        guard id != 0, width > 0, height > 0 else {
            throw ResourceAllocationError.invalidDimensions(width: width, height: height)
        }
        guard let wireFormat = VirtioGPU.Format(rawValue: format),
              wireFormat.isSupportedScanout32Bit
        else { throw ResourceAllocationError.unsupportedFormat(format) }

        let widthInt = Int(width)
        let heightInt = Int(height)
        let (tightStride, strideOverflow) = widthInt.multipliedReportingOverflow(by: 4)
        guard !strideOverflow else {
            throw ResourceAllocationError.invalidDimensions(width: width, height: height)
        }
        let hostStride = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, tightStride)
        let (minimumBytes, sizeOverflow) = hostStride.multipliedReportingOverflow(by: heightInt)
        guard !sizeOverflow, minimumBytes > 0 else {
            throw ResourceAllocationError.invalidDimensions(width: width, height: height)
        }

        guard let surface = IOSurface(properties: [
            .width: widthInt,
            .height: heightInt,
            .bytesPerElement: 4,
            .bytesPerRow: hostStride,
            .allocSize: minimumBytes,
            .pixelFormat: kCVPixelFormatType_32BGRA,
        ]) else {
            throw ResourceAllocationError.allocationFailed(bytes: minimumBytes)
        }
        let ref = surface as IOSurfaceRef
        let byteCount = IOSurfaceGetAllocSize(ref)
        guard byteCount >= minimumBytes else {
            throw ResourceAllocationError.tooSmall(
                allocated: byteCount, requested: minimumBytes)
        }
        let geometry = VirtioGPU.BlobGeometry(
            width: widthInt, height: heightInt, bytesPerRow: hostStride)
        let metadata = Resource2DMetadata(
            format: format, width: widthInt, height: heightInt,
            sourceBytesPerRow: tightStride)
        let resource = GPUResource(
            resourceID: id,
            pointer: IOSurfaceGetBaseAddress(ref),
            surface: ref,
            byteCount: byteCount,
            geometry: geometry,
            twoDimensional: metadata,
            blobID: 0)
        resources[id] = resource
        return resource
    }

    /// Mesa Venus blob: adopt the host pointer virglrenderer already mapped.
    /// The guest will see these pages through the aperture. We do not own them.
    @discardableResult
    public func adoptHostMapping(
        id: UInt32, pointer: UnsafeMutableRawPointer, size: UInt64, blobID: UInt64
    ) throws -> GPUResource {
        guard resources[id] == nil else { throw ResourceAllocationError.duplicateID(id) }
        let byteCount = try alignedByteCount(for: size)
        let page = UInt(pageSize)
        guard UInt(bitPattern: pointer) % page == 0 else {
            throw ResourceAllocationError.unalignedPointer
        }
        let resource = GPUResource(
            resourceID: id, pointer: pointer, surface: nil, byteCount: byteCount,
            geometry: nil, blobID: blobID)
        resources[id] = resource
        return resource
    }

    /// DEVICE_LOCAL Venus image: MTLHeap only, no CPU mapping into the aperture.
    @discardableResult
    public func adoptMetalHeapBlob(
        id: UInt32, size: UInt64, blobID: UInt64
    ) throws -> GPUResource {
        guard resources[id] == nil else { throw ResourceAllocationError.duplicateID(id) }
        let byteCount = try alignedByteCount(for: size)
        let resource = GPUResource(
            resourceID: id, pointer: nil, surface: nil, byteCount: byteCount,
            geometry: nil, blobID: blobID)
        resources[id] = resource
        return resource
    }

    @discardableResult
    public func adoptVirglResource(id: UInt32) throws -> GPUResource {
        guard resources[id] == nil else { throw ResourceAllocationError.duplicateID(id) }
        guard id != 0 else { throw ResourceAllocationError.unknownID(id) }
        let resource = GPUResource(
            resourceID: id, pointer: nil, surface: nil, byteCount: 0,
            geometry: nil, blobID: 0, isVirglResource: true)
        resources[id] = resource
        return resource
    }

    public func require(_ id: UInt32) throws -> GPUResource {
        guard let resource = resources[id] else { throw ResourceAllocationError.unknownID(id) }
        return resource
    }

    @discardableResult
    public func remove(_ id: UInt32) -> GPUResource? {
        resources.removeValue(forKey: id)
    }

    public func removeAll() {
        resources.removeAll(keepingCapacity: true)
    }

    public var mapped: [GPUResource] {
        resources.values.filter { $0.mappedOffset != nil }
    }

}
