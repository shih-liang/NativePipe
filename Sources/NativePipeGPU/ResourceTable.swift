import Foundation
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
    public let byteCount: Int
    /// Mesa's Venus object id, or the packed geometry word for a window.
    public let blobID: UInt64
    /// Standard VirGL RESOURCE_CREATE_3D resource owned by virglrenderer.
    public let isVirglResource: Bool
    /// Renderer-provided `VIRTIO_GPU_MAP_CACHE_*` value for mappable blobs.
    public let mapInfo: UInt32

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
        byteCount: Int,
        blobID: UInt64,
        mapInfo: UInt32 = 0,
        isVirglResource: Bool = false
    ) {
        self.resourceID = resourceID
        self.pointer = pointer
        self.byteCount = byteCount
        self.blobID = blobID
        self.mapInfo = mapInfo
        self.isVirglResource = isVirglResource
    }
}

public enum ResourceAllocationError: LocalizedError {
    case duplicateID(UInt32)
    case unknownID(UInt32)
    case emptyBlob
    case blobTooLarge(UInt64)
    case unalignedPointer

    public var errorDescription: String? {
        switch self {
        case .duplicateID(let id): return "resource \(id) already exists"
        case .unknownID(let id): return "no such resource \(id)"
        case .emptyBlob: return "blob size must be greater than zero"
        case .blobTooLarge(let bytes): return "blob size \(bytes) exceeds host address space"
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

    public var all: [GPUResource] { Array(resources.values) }

    public subscript(id: UInt32) -> GPUResource? { resources[id] }

    private func alignedByteCount(for size: UInt64) throws -> Int {
        guard size > 0 else { throw ResourceAllocationError.emptyBlob }
        guard size <= UInt64(Int.max - (pageSize - 1)) else {
            throw ResourceAllocationError.blobTooLarge(size)
        }
        return (Int(size) + pageSize - 1) / pageSize * pageSize
    }

    /// Mesa Venus blob: adopt the host pointer virglrenderer already mapped.
    /// The guest will see these pages through the aperture. We do not own them.
    @discardableResult
    public func adoptHostMapping(
        id: UInt32, pointer: UnsafeMutableRawPointer, size: UInt64, blobID: UInt64,
        mapInfo: UInt32
    ) throws -> GPUResource {
        guard resources[id] == nil else { throw ResourceAllocationError.duplicateID(id) }
        let byteCount = try alignedByteCount(for: size)
        let page = UInt(pageSize)
        guard UInt(bitPattern: pointer) % page == 0 else {
            throw ResourceAllocationError.unalignedPointer
        }
        let resource = GPUResource(
            resourceID: id, pointer: pointer, byteCount: byteCount, blobID: blobID,
            mapInfo: mapInfo)
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
            resourceID: id, pointer: nil, byteCount: byteCount, blobID: blobID)
        resources[id] = resource
        return resource
    }

    @discardableResult
    public func adoptVirglResource(id: UInt32) throws -> GPUResource {
        guard resources[id] == nil else { throw ResourceAllocationError.duplicateID(id) }
        guard id != 0 else { throw ResourceAllocationError.unknownID(id) }
        let resource = GPUResource(
            resourceID: id, pointer: nil, byteCount: 0,
            blobID: 0, isVirglResource: true)
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
