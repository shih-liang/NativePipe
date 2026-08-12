import CoreVideo
import Foundation
import IOSurface
import Metal
import os

/// One guest `resource_id` on the host virtio-gpu.
///
/// Two kinds of HOST3D blob, both mapped into the guest through the same
/// aperture:
///
///   * Compositor (guest `nativepipe-wayland`): the host allocated an
///     IOSurface. That object is the window — `CALayer.contents` and the
///     guest mmap see the same pages.
///   * Mesa Venus (guest Mesa, not us): vkr already allocated the memory
///     during SUBMIT_3D. We hold the pointer virglrenderer mapped.
///     The window presents it on a CAMetalLayer — same as native macOS
///     Vulkan. No second allocation.
public final class GPUResource {
    public let resourceID: UInt32
    /// Set for compositor / window blobs. Nil for a Venus-only mapping.
    public let surface: IOSurfaceRef?
    public let byteCount: Int
    public let geometry: VirtioGPU.BlobGeometry?
    /// Mesa's Venus object id, or the packed geometry word for a window.
    public let blobID: UInt64

    /// Offset inside the host-visible shared memory region, once the guest has
    /// asked for it with `RESOURCE_MAP_BLOB`. Nil while unmapped.
    public internal(set) var mappedOffset: UInt64?

    /// Contexts currently holding a reference, from `CTX_ATTACH_RESOURCE`.
    public internal(set) var attachedContexts: Set<UInt32> = []

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
        blobID: UInt64
    ) {
        self.resourceID = resourceID
        self.pointer = pointer
        self.surface = surface
        self.byteCount = byteCount
        self.geometry = geometry
        self.blobID = blobID
    }

    /// A Metal buffer over the exact pages mapped into the guest aperture.
    public func makeMetalBuffer(using device: MTLDevice) -> MTLBuffer? {
        guard let pointer else { return nil }
        return device.makeBuffer(
            bytesNoCopy: pointer,
            length: byteCount,
            options: .storageModeShared,
            deallocator: nil)
    }

    /// A BGRA texture view over a shaped window resource.
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

public enum ResourceAllocationError: LocalizedError {
    case duplicateID(UInt32)
    case unknownID(UInt32)
    case allocationFailed(bytes: Int)
    case emptyBlob
    case tooSmall(allocated: Int, requested: Int)
    case unalignedPointer

    public var errorDescription: String? {
        switch self {
        case .duplicateID(let id): return "resource \(id) already exists"
        case .unknownID(let id): return "no such resource \(id)"
        case .allocationFailed(let bytes): return "could not allocate \(bytes) bytes"
        case .emptyBlob: return "blob size must be greater than zero"
        case .tooSmall(let allocated, let requested):
            return "allocated \(allocated) bytes for a blob the guest will map \(requested) of"
        case .unalignedPointer: return "host mapping is not a multiple of the host page"
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

    public subscript(id: UInt32) -> GPUResource? { resources[id] }

    /// Window / compositor blob: the host allocates an IOSurface.
    @discardableResult
    public func createBlob(
        id: UInt32, size: UInt64, geometry: VirtioGPU.BlobGeometry? = nil,
        blobID: UInt64 = 0
    ) throws -> GPUResource {
        guard resources[id] == nil else { throw ResourceAllocationError.duplicateID(id) }
        guard size > 0 else { throw ResourceAllocationError.emptyBlob }

        let requested = (Int(size) + pageSize - 1) / pageSize * pageSize

        var properties: [IOSurfacePropertyKey: Any] = [
            .bytesPerElement: 4,
            .pixelFormat: kCVPixelFormatType_32BGRA,
        ]
        if let geometry, geometry.byteCount <= requested {
            let aligned = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, geometry.bytesPerRow)
            if aligned != geometry.bytesPerRow {
                ResourceTable.log.error(
                    "resource \(id) has bytesPerRow \(geometry.bytesPerRow); the display pipeline wants \(aligned) and will show nothing")
            }
            properties[.width] = geometry.width
            properties[.height] = geometry.height
            properties[.bytesPerRow] = geometry.bytesPerRow
        } else {
            properties[.width] = requested / 4
            properties[.height] = 1
            properties[.bytesPerRow] = requested
            properties[.allocSize] = requested
        }

        guard let surface = IOSurface(properties: properties) else {
            throw ResourceAllocationError.allocationFailed(bytes: requested)
        }
        let ref = surface as IOSurfaceRef
        let byteCount = IOSurfaceGetAllocSize(ref)
        guard byteCount >= requested else {
            throw ResourceAllocationError.tooSmall(allocated: byteCount, requested: requested)
        }
        let pointer = IOSurfaceGetBaseAddress(ref)

        let resource = GPUResource(
            resourceID: id, pointer: pointer, surface: ref, byteCount: byteCount,
            geometry: geometry, blobID: blobID)
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
        guard size > 0 else { throw ResourceAllocationError.emptyBlob }
        let page = UInt(pageSize)
        guard UInt(bitPattern: pointer) % page == 0 else {
            throw ResourceAllocationError.unalignedPointer
        }
        let byteCount = (Int(size) + pageSize - 1) / pageSize * pageSize
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
        guard size > 0 else { throw ResourceAllocationError.emptyBlob }
        let byteCount = (Int(size) + pageSize - 1) / pageSize * pageSize
        let resource = GPUResource(
            resourceID: id, pointer: nil, surface: nil, byteCount: byteCount,
            geometry: nil, blobID: blobID)
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

    public var mapped: [GPUResource] {
        resources.values.filter { $0.mappedOffset != nil }
    }
}
