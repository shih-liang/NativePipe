import Darwin
import Foundation
import NativePipeVenus
import Virtualization
import os

/// Host virtio-gpu. Guest: stock `virtio_gpu.ko` + Mesa VirGL/Venus + the
/// NativePipe compositor. The product exposes VirGL for OpenGL/GLES and Venus
/// for Vulkan with zero KMS scanouts; Apple's separate Virtio 2D device owns
/// the optional full-VM framebuffer.
///
///   * CREATE_BLOB from Mesa Venus → ordinary renderer allocations. Wayland
///     linux-dmabuf commits continue naming those original client textures.
///   * The host retains each scene source while Metal composites it directly
///     into a CAMetalDrawable, then releases the source on GPU completion.
///   * VirGL SUBMIT_3D → vrend → ANGLE/EGL → Metal.
///   * Venus SUBMIT_3D → vkr → MoltenVK → Metal. Guest Venus remains
///     inside Mesa and is not implemented here.
///
/// All delegate callbacks arrive on `deviceQueue`, so the state below is queue
/// confined and deliberately unsynchronised.
@available(macOS 27.0, *)
public final class VirtioGPUDevice: NSObject, @unchecked Sendable {
    private static let log = Logger(subsystem: "com.nativepipe.gpu", category: "virtio-gpu")
    private static let deviceQueueKey = DispatchSpecificKey<Void>()

    /// Resource lifecycle tracing, off unless NATIVEPIPE_GPU_TRACE is set.
    /// Writes to stderr so it interleaves with `nativepipe smoke` output.
    private static let trace = ProcessInfo.processInfo.environment["NATIVEPIPE_GPU_TRACE"] != nil

    /// Written into the first words of every fresh blob so the guest can prove
    /// it is reading host memory rather than zeroed guest pages. Mirrored in
    /// `guest/tools/blobtest.c`.

    private static func note(_ message: @autoclosure () -> String) {
        guard trace else { return }
        FileHandle.standardError.write(Data("[gpu] \(message())\n".utf8))
    }

    /// First few 32-bit words of a resource, for the probe.
    private static func peek(_ resource: GPUResource) -> String {
        guard resource.isHostMappable, let address = resource.baseAddress else {
            return "<metal-heap>"
        }
        let words = address.bindMemory(to: UInt32.self, capacity: 4)
        return (0..<4).map { String(format: "%08x", words[$0]) }.joined(separator: " ")
    }

    public let configuration: VZCustomVirtioDeviceConfiguration
    public let deviceQueue: DispatchQueue

    private var device: VZCustomVirtioDevice?
    private var hostVisibleRegion: VZVirtioSharedMemoryRegion?
    private let resources = ResourceTable()
    private var contexts: Set<UInt32> = []
    /// Removed from the guest-visible table but retained until VZ has really
    /// unmapped its aperture range and Venus has dropped the import.
    private var retiringResources: [UInt32: GPUResource] = [:]
    /// A failed VZ unmap cannot be treated as success: the guest mapping may
    /// still reference the backing. Keep it quarantined until a later reset can
    /// retry, or until the VM stops and VZ tears the whole region down.
    private var quarantinedResourceIDs: Set<UInt32> = []
    private var deferredBlobCreates: [UInt32: DeferredBlobCreate] = [:]
    /// Owns the import table and, when present, virglrenderer. Created on the
    /// device queue's thread in `init` so every later call stays there.
    private let venus: OpaquePointer
    /// Stable virtual-hardware ABI. A missing backend fails device creation;
    /// it never changes the capset list seen by an existing VM definition.
    private static let advertisedCapsets: [VirtioGPU.Capset] = [.virgl, .virgl2, .venus]
    /// Set once Venus/virglrenderer has been cleaned up. Further virtio
    /// commands are dropped; `deinit` must not call cleanup again.
    private var rendererTornDown = false
    private var rendererDestroyed = false

    /// Resource lookup for consumers outside the device queue — the window
    /// bridge resolves a committed frame's `resourceID` on the main actor.
    /// `ResourceTable` itself stays queue confined; this is a published mirror.
    private let publishedLock = NSLock()
    private var published: [UInt32: PublishedBuffer] = [:]
    private struct CachedMetalTexture {
        let width: Int
        let height: Int
        let bytesPerRow: Int
        let format: UInt32
        let texture: AnyObject
    }
    private var metalTextures: [UInt32: CachedMetalTexture] = [:]

    /// Published host view of one renderer resource. Application windows
    /// resolve the compositor's original Metal texture by resource id.
    private struct PublishedBuffer {
        var byteCount: Int
        /// The renderer owns the allocation and validates its exact texture
        /// geometry when exporting a native handle; it has no CPU byte range.
        var isRendererNative: Bool
    }

    public struct MetalTextureRequest: Sendable {
        public let resourceID: UInt32
        public let width: Int
        public let height: Int
        public let bytesPerRow: Int
        public let format: UInt32

        public init(
            resourceID: UInt32, width: Int, height: Int,
            bytesPerRow: Int, format: UInt32
        ) {
            self.resourceID = resourceID
            self.width = width
            self.height = height
            self.bytesPerRow = bytesPerRow
            self.format = format
        }
    }

    public enum MetalTextureStatus: Sendable {
        case ready
        case unpublished
        case unavailable
    }

    /// Result for one exact committed Vulkan image. `unpublished` is the only
    /// retryable state: its CREATE_BLOB has not reached the host yet. Once the
    /// resource is published, failure to export its real Metal texture is
    /// terminal for that commit; treating the blob's raw storage as a linear
    /// image would ignore the renderer's image layout and aliasing rules.
    public struct MetalTextureResolution: @unchecked Sendable {
        public let status: MetalTextureStatus
        public let texture: AnyObject?

        public init(status: MetalTextureStatus, texture: AnyObject? = nil) {
            self.status = status
            self.texture = texture
        }
    }

    /// Whether this id currently names a live host resource. Unknown ids are
    /// treated as the CREATE_BLOB/window-channel ordering case by WindowBridge.
    public func isResourcePublished(_ resourceID: UInt32) -> Bool {
        publishedLock.lock()
        defer { publishedLock.unlock() }
        return published[resourceID] != nil
    }

    /// A Venus image exported as an MTLTexture. Application windows sample it
    /// directly while rendering into their CAMetalDrawable.
    public func gpuMetalTexture(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> AnyObject? {
        let lookup = { [self] in
            metalTextureResolutionOnDeviceQueue(
                forResource: resourceID, width: width, height: height,
                bytesPerRow: bytesPerRow, format: format).texture
        }
        return DispatchQueue.getSpecific(key: Self.deviceQueueKey) != nil
            ? lookup() : deviceQueue.sync(execute: lookup)
    }

    /// Resolves one atomic window scene without blocking AppKit's main actor.
    /// The completion is delivered on the main queue in request order.
    public func gpuMetalTextures(
        for requests: [MetalTextureRequest],
        completion: @escaping @MainActor ([MetalTextureResolution]) -> Void
    ) {
        deviceQueue.async { [weak self] in
            guard let self, !self.rendererTornDown else {
                DispatchQueue.main.async {
                    completion(requests.map { _ in
                        MetalTextureResolution(status: .unavailable)
                    })
                }
                return
            }
            let textures = requests.map {
                self.metalTextureResolutionOnDeviceQueue(
                    forResource: $0.resourceID,
                    width: $0.width, height: $0.height,
                    bytesPerRow: $0.bytesPerRow, format: $0.format)
            }
            DispatchQueue.main.async { completion(textures) }
        }
    }

    private func metalTextureResolutionOnDeviceQueue(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> MetalTextureResolution {
        publishedLock.lock()
        if let cached = metalTextures[resourceID],
           cached.width == width, cached.height == height,
           cached.bytesPerRow == bytesPerRow, cached.format == format,
           published[resourceID] != nil {
            publishedLock.unlock()
            return MetalTextureResolution(status: .ready, texture: cached.texture)
        }
        let entry = published[resourceID]
        publishedLock.unlock()
        guard let entry else {
            return MetalTextureResolution(status: .unpublished)
        }
        let (minimumBytesPerRow, rowOverflow) = width.multipliedReportingOverflow(by: 4)
        let (requiredBytes, imageOverflow) = bytesPerRow.multipliedReportingOverflow(by: height)
        guard !rendererTornDown, width > 0, height > 0,
              !rowOverflow, !imageOverflow,
              bytesPerRow >= minimumBytesPerRow,
              (entry.isRendererNative || entry.byteCount >= requiredBytes),
              format == 1 || format == 67
        else {
            return MetalTextureResolution(status: .unavailable)
        }

        guard let raw = np_venus_metal_texture(
            venus, resourceID, UInt32(width), UInt32(height),
            UInt32(bytesPerRow), format)
        else {
            return MetalTextureResolution(status: .unavailable)
        }
        let texture = Unmanaged<AnyObject>.fromOpaque(raw).takeUnretainedValue()
        publishedLock.lock()
        if published[resourceID] != nil {
            metalTextures[resourceID] = CachedMetalTexture(
                width: width, height: height, bytesPerRow: bytesPerRow,
                format: format, texture: texture)
        }
        publishedLock.unlock()
        return MetalTextureResolution(status: .ready, texture: texture)
    }

    private func buffer(forResource resourceID: UInt32) -> PublishedBuffer? {
        publishedLock.lock()
        defer { publishedLock.unlock() }
        return published[resourceID]
    }

    private func publish(_ resource: GPUResource) {
        let entry = PublishedBuffer(
            byteCount: resource.byteCount,
            isRendererNative: resource.isVirglResource)
        publishedLock.lock()
        metalTextures.removeValue(forKey: resource.resourceID)
        published[resource.resourceID] = entry
        publishedLock.unlock()
    }

	private func cancelDeferredBlobCreates() {
		let deferred = deferredBlobCreates.values
		deferredBlobCreates.removeAll(keepingCapacity: false)
		for create in deferred { create.element.returnToQueue() }
	}

    private func unpublish(_ resourceID: UInt32) {
        publishedLock.lock()
        published.removeValue(forKey: resourceID)
        metalTextures.removeValue(forKey: resourceID)
        publishedLock.unlock()
    }

    /// Aperture operations, run strictly one at a time.
    ///
    /// `mapMemory` and `unmapMemory` complete asynchronously, but the guest
    /// reuses an offset as soon as it has freed it — during a window resize the
    /// same range is unmapped and remapped within a frame. Issuing the map while
    /// the unmap is still in flight leaves two operations overlapping the same
    /// range, and which one wins depends on timing, which is exactly what an
    /// intermittently blank window looks like.
    private typealias RegionOperation = (@escaping () -> Void) -> Void
    private var regionQueue: [RegionOperation] = []
    private var regionBusy = false

    private func enqueueRegionOperation(_ operation: @escaping RegionOperation) {
        regionQueue.append(operation)
        pumpRegionQueue()
    }

    private func pumpRegionQueue() {
        guard !regionBusy, !regionQueue.isEmpty else { return }
        regionBusy = true
        let operation = regionQueue.removeFirst()
        operation { [weak self] in
            guard let self else { return }
            self.regionBusy = false
            self.pumpRegionQueue()
        }
    }

    /// Fired once the guest driver has completed negotiation and set DRIVER_OK.
    public var onDriverReady: ((VZNegotiatedVirtioFeatureSet?) -> Void)?
    /// Fired on the main queue after a Venus blob is adopted and published.
    public var onResourcePublished: ((UInt32) -> Void)?

	private struct DeferredBlobCreate {
		let request: VirtioGPU.ResourceCreateBlob
		let header: VirtioGPU.ControlHeader
		let element: VZVirtioQueueElement
	}

    private struct GuestBackingSegment {
        let logicalOffset: UInt64
        let mapping: VZGuestMemoryMapping
    }

    private struct GuestBacking {
        let segments: [GuestBackingSegment]
        let byteCount: UInt64
    }

    private enum BackingFailure: LocalizedError {
        case missingDevice
        case emptyBacking
        case backingAlreadyAttached
        case invalidBackingRange
        case guestMappingFailed(address: UInt64, length: UInt32)

        var errorDescription: String? {
            switch self {
            case .missingDevice: return "custom virtio device is not ready"
            case .emptyBacking: return "resource backing is empty"
            case .backingAlreadyAttached: return "resource backing is already attached"
            case .invalidBackingRange: return "resource backing range overflow"
            case .guestMappingFailed(let address, let length):
                return "could not map guest range 0x\(String(address, radix: 16))+\(length)"
            }
        }
    }

    private var guestBackings: [UInt32: GuestBacking] = [:]

    /// Address space only, so the default is generous rather than frugal.
    public static let defaultApertureSize: UInt64 = 4 << 30

    /// Size of the host-visible aperture this device advertises.
    public let apertureSize: UInt64

    public enum InitializationError: LocalizedError {
        case rendererUnavailable([VirtioGPU.Capset])

        public var errorDescription: String? {
            switch self {
            case .rendererUnavailable(let capsets):
                let names = capsets.map(String.init(describing:)).joined(separator: ", ")
                return "NativePipe renderer runtime failed to initialize required capsets: \(names)"
            }
        }
    }

    public init(
        hostVisibleApertureSize: UInt64 = VirtioGPUDevice.defaultApertureSize
    ) throws {
        apertureSize = hostVisibleApertureSize
        deviceQueue = DispatchQueue(label: "com.nativepipe.gpu.device", qos: .userInteractive)

        let configuration = VZCustomVirtioDeviceConfiguration()
        configuration.deviceID = VirtioGPU.deviceID
        configuration.pciClassID = VirtioGPU.pciClassDisplay
        configuration.pciSubclassID = VirtioGPU.pciSubclassOther
        configuration.virtioQueueCount = VirtioGPU.queueCount

        guard let created = np_venus_create() else {
            throw InitializationError.rendererUnavailable(Self.advertisedCapsets)
        }
        let missingCapsets = Self.advertisedCapsets.filter { capset in
            var version: UInt32 = 0
            var size: UInt32 = 0
            np_renderer_capset_info(created, capset.rawValue, &version, &size)
            return size == 0
        }
        guard missingCapsets.isEmpty else {
            np_venus_destroy(created)
            throw InitializationError.rendererUnavailable(missingCapsets)
        }

        // Offered but not demanded. Making these mandatory would refuse to start
        // the device against a guest kernel too old to accept them; offering them
        // instead lets any virtio_gpu driver bind, and the negotiated set tells
        // us afterwards what we actually got.
        let features: [UInt32] = [
            VirtioGPU.Feature.virgl,
            VirtioGPU.Feature.resourceBlob,
            VirtioGPU.Feature.contextInit,
        ]
        configuration.optionalFeatures.subset0 = VirtioGPU.Feature.mask(features)

        let deviceConfig = VirtioGPU.DeviceConfig(
            numScanouts: 0,
            numCapsets: UInt32(Self.advertisedCapsets.count))
        configuration.deviceSpecificConfiguration =
            VZVirtioDeviceSpecificConfiguration(configurationData: deviceConfig.encoded())

        configuration.sharedMemoryRegions = [
            VZVirtioSharedMemoryRegionConfiguration(
                regionID: VirtioGPU.hostVisibleRegionID, size: hostVisibleApertureSize)
        ]

        self.configuration = configuration
        self.venus = created
        super.init()
        deviceQueue.setSpecific(key: Self.deviceQueueKey, value: ())
        Self.log.info("venus host live=\(np_venus_is_live(created))")

        configuration.provider = VZCustomVirtioDeviceDelegateProvider(
            deviceQueue: deviceQueue, delegate: self)
    }

    deinit {
        // Prefer `customVirtioDeviceWillStop`, which runs on `deviceQueue` and
        // tears Venus down before AppKit's main actor releases this object.
        // Falling through to cleanup here used to `thrd_join` the in-process
        // render thread on the main thread and beachball the app.
        if !rendererDestroyed {
            np_venus_destroy(venus)
        }
    }

    /// Schedule cleanup for an unexpected stop path that did not receive the
    /// custom-device `willStop` callback. Normal stop/restart cleans up there.
    public func requestRendererShutdown() {
        if DispatchQueue.getSpecific(key: Self.deviceQueueKey) != nil {
            shutdownRenderer()
        } else {
            deviceQueue.async { [self] in shutdownRenderer() }
        }
    }

    // MARK: - Command dispatch

    private func handle(_ element: VZVirtioQueueElement, queueIndex: UInt16) {
        // One access, then parse from the copy. Re-reading the descriptor would
        // let the guest change a field between validation and use.
        let request = Data(element.readBuffers().joined())
        var reader = LittleEndianReader(request)

        guard let header = try? VirtioGPU.ControlHeader(parsing: &reader) else {
            Self.log.error("dropped a command shorter than its header")
            element.returnToQueue()
            return
        }

        guard let command = header.command else {
            Self.log.error("unknown virtio-gpu command 0x\(String(header.type, radix: 16))")
            respond(element, header.reply(.errUnspecified))
            return
        }

        do {
            try dispatch(command, header: header, reader: &reader, element: element,
                         queueIndex: queueIndex)
        } catch {
            Self.log.error(
                "\(String(describing: command), privacy: .public) failed: \(error.localizedDescription, privacy: .public)")
            Self.note("reject  \(command): \(error.localizedDescription)")
            respond(element, header.reply(.errInvalidParameter))
        }
    }

    private func dispatch(
        _ command: VirtioGPU.CommandType,
        header: VirtioGPU.ControlHeader,
        reader: inout LittleEndianReader,
        element: VZVirtioQueueElement,
        queueIndex: UInt16
    ) throws {
        switch command {

        case .getDisplayInfo:
            let info = VirtioGPU.DisplayInfoResponse(
                width: 0, height: 0, enabled: false)
            respond(element, header.reply(.okDisplayInfo), body: info.encoded())

        case .resourceCreate2D:
            _ = try VirtioGPU.ResourceCreate2D(parsing: &reader)
            respond(element, header.reply(.errUnspecified))

        case .resourceAttachBacking:
            let request = try VirtioGPU.ResourceAttachBacking(parsing: &reader)
            let resource = try resources.require(request.resourceID)
            let backing = try makeGuestBacking(request.entries, for: request.resourceID)
            if resource.isVirglResource {
                let entries = backing.segments.map {
                    np_renderer_iovec(base: $0.mapping.mutableBytes, length: $0.mapping.length)
                }
                let rc = entries.withUnsafeBufferPointer {
                    np_renderer_resource_attach_iov(
                        venus, request.resourceID, $0.baseAddress, UInt32($0.count))
                }
                guard rc == 0 else {
                    throw BackingFailure.invalidBackingRange
                }
            }
            guestBackings[request.resourceID] = backing
            Self.note(
                "backing res=\(request.resourceID) entries=\(request.entries.count) " +
                "bytes=\(backing.byteCount)")
            respond(element, header.reply(.okNoData))

        case .resourceDetachBacking:
            let request = try VirtioGPU.ResourceDetachBacking(parsing: &reader)
            let resource = try resources.require(request.resourceID)
            if resource.isVirglResource {
                np_renderer_resource_detach_iov(venus, request.resourceID)
            }
            guestBackings.removeValue(forKey: request.resourceID)
            respond(element, header.reply(.okNoData))

        case .transferToHost2D:
            _ = try VirtioGPU.TransferToHost2D(parsing: &reader)
            respond(element, header.reply(.errUnspecified))

        case .setScanout:
            _ = try VirtioGPU.SetScanout(parsing: &reader)
            respond(element, header.reply(.errInvalidScanoutID))

        case .setScanoutBlob:
            _ = try VirtioGPU.SetScanoutBlob(parsing: &reader)
            respond(element, header.reply(.errInvalidScanoutID))

        case .resourceFlush:
            let request = try VirtioGPU.ResourceFlush(parsing: &reader)
            _ = try resources.require(request.resourceID)
            respond(element, header.reply(.okNoData))

        case .getCapsetInfo:
            let request = try VirtioGPU.GetCapsetInfo(parsing: &reader)
            guard request.capsetIndex < UInt32(Self.advertisedCapsets.count) else {
                respond(element, header.reply(.errInvalidParameter))
                return
            }
            let capset = Self.advertisedCapsets[Int(request.capsetIndex)]
            var maxVersion: UInt32 = 0
            var maxSize: UInt32 = 0
            np_renderer_capset_info(venus, capset.rawValue, &maxVersion, &maxSize)
            let info = VirtioGPU.CapsetInfoResponse(
                capsetID: capset.rawValue,
                capsetMaxVersion: maxVersion,
                capsetMaxSize: maxSize)
            respond(element, header.reply(.okCapsetInfo), body: info.encoded())

        case .getCapset:
            let request = try VirtioGPU.GetCapset(parsing: &reader)
            guard Self.advertisedCapsets.contains(where: { $0.rawValue == request.capsetID }) else {
                respond(element, header.reply(.errInvalidParameter))
                return
            }
            var maxVersion: UInt32 = 0
            var maxSize: UInt32 = 0
            np_renderer_capset_info(venus, request.capsetID, &maxVersion, &maxSize)
            guard maxSize > 0 else {
                respond(element, header.reply(.okCapset))
                return
            }
            var blob = Data(count: Int(maxSize))
            let written = blob.withUnsafeMutableBytes { raw -> UInt32 in
                np_renderer_fill_caps(
                    venus, request.capsetID, request.capsetVersion,
                    raw.baseAddress, maxSize)
            }
            respond(element, header.reply(.okCapset), body: blob.prefix(Int(written)))

        case .ctxCreate:
            let request = try VirtioGPU.ContextCreate(parsing: &reader)
            guard !contexts.contains(header.contextID) else {
                respond(element, header.reply(.errInvalidContextID))
                return
            }
            let rc = np_venus_context_create(
                venus, header.contextID, request.capsetID, request.debugName)
            guard rc == 0 else {
                respond(element, header.reply(.errInvalidParameter))
                return
            }
            contexts.insert(header.contextID)
            Self.log.info(
                "context \(header.contextID) created for capset \(request.capsetID) (\(request.debugName, privacy: .public))")
            respond(element, header.reply(.okNoData))

        case .ctxDestroy:
            guard contexts.remove(header.contextID) != nil else {
                respond(element, header.reply(.errInvalidContextID))
                return
            }
            np_venus_context_destroy(venus, header.contextID)
            respond(element, header.reply(.okNoData))

        case .ctxAttachResource:
            let request = try VirtioGPU.ContextResource(parsing: &reader)
            guard contexts.contains(header.contextID) else {
                respond(element, header.reply(.errInvalidContextID))
                return
            }
            _ = try resources.require(request.resourceID)
            let rc = np_venus_attach(venus, header.contextID, request.resourceID)
            respond(element, header.reply(rc == 0 ? .okNoData : .errInvalidParameter))

        case .ctxDetachResource:
            let request = try VirtioGPU.ContextResource(parsing: &reader)
            guard contexts.contains(header.contextID) else {
                respond(element, header.reply(.errInvalidContextID))
                return
            }
            _ = try resources.require(request.resourceID)
            let rc = np_venus_detach(venus, header.contextID, request.resourceID)
            respond(element, header.reply(rc == 0 ? .okNoData : .errInvalidParameter))

        case .resourceCreateBlob:
            let request = try VirtioGPU.ResourceCreateBlob(parsing: &reader)
            guard request.blobMemory == VirtioGPU.BlobMemory.host3D.rawValue else {
                // Guest-memory blobs would put the pixels in guest RAM, which is
                // the copy this whole design exists to avoid.
                Self.log.error("refusing blob_mem \(request.blobMemory); only HOST3D is supported")
                respond(element, header.reply(.errInvalidParameter))
                return
            }
            if resources[request.resourceID] != nil {
                Self.log.error("duplicate live resource id \(request.resourceID)")
                respond(element, header.reply(.errInvalidResourceID))
                return
            }
            if retiringResources[request.resourceID] != nil {
                guard !quarantinedResourceIDs.contains(request.resourceID) else {
                    respond(element, header.reply(.errInvalidResourceID))
                    return
                }
                guard deferredBlobCreates[request.resourceID] == nil else {
                    respond(element, header.reply(.errInvalidResourceID))
                    return
                }
                deferredBlobCreates[request.resourceID] = DeferredBlobCreate(
                    request: request, header: header, element: element)
                return
            }
            createBlobResource(request, header: header, element: element)

        case .resourceMapBlob:
            let request = try VirtioGPU.ResourceMapBlob(parsing: &reader)
            let resource = try resources.require(request.resourceID)
            try mapIntoGuest(resource, atOffset: request.offset, element: element, header: header)

        case .resourceUnmapBlob:
            let request = try VirtioGPU.ResourceUnmapBlob(parsing: &reader)
            let resource = try resources.require(request.resourceID)
            unmapFromGuest(resource, element: element, header: header)

        case .resourceUnref:
            let request = try VirtioGPU.ResourceUnref(parsing: &reader)
            if let resource = resources[request.resourceID] {
                Self.note("unref   res=\(resource.resourceID) final \(Self.peek(resource))")
            }
            unpublish(request.resourceID)
            guard let resource = resources.remove(request.resourceID) else {
                respond(element, header.reply(.okNoData))
                return
            }
            if resource.isVirglResource {
                np_venus_unimport_blob(venus, request.resourceID)
                guestBackings.removeValue(forKey: request.resourceID)
                respond(element, header.reply(.okNoData))
                return
            }
			guestBackings.removeValue(forKey: request.resourceID)
			retiringResources[request.resourceID] = resource
            unmapFromGuest(resource, element: nil, header: nil) { [weak self] success in
                guard let self, !self.rendererTornDown else {
                    element.returnToQueue()
                    return
                }
                guard success else {
                    self.quarantinedResourceIDs.insert(request.resourceID)
                    self.respond(element, header.reply(.errUnspecified))
                    if let deferred = self.deferredBlobCreates.removeValue(
                        forKey: request.resourceID) {
                        self.respond(
                            deferred.element,
                            deferred.header.reply(.errInvalidResourceID))
                    }
                    return
                }
                np_venus_unimport_blob(self.venus, request.resourceID)
                self.retiringResources.removeValue(forKey: request.resourceID)
                self.quarantinedResourceIDs.remove(request.resourceID)
                self.respond(element, header.reply(.okNoData))
				if let deferred = self.deferredBlobCreates.removeValue(
					forKey: request.resourceID) {
					self.createBlobResource(
						deferred.request, header: deferred.header,
						element: deferred.element)
				}
            }

        case .submit3D:
            let request = try VirtioGPU.Submit3D(parsing: &reader)
            let payload = try reader.readBytes(Int(request.payloadByteCount))
            submitVenus(payload, header: header, element: element)

        case .getEDID:
            // VIRTIO_GPU_F_EDID is intentionally not offered. A guest issuing
            // the command anyway gets a protocol error rather than invented data.
            respond(element, header.reply(.errUnspecified))

        case .updateCursor, .moveCursor:
            // Cursor lives on the macOS side; nothing to draw here.
            respond(element, header.reply(.okNoData))

        case .resourceCreate3D:
            let request = try VirtioGPU.ResourceCreate3D(parsing: &reader)
            Self.note(
                "create3d res=\(request.resourceID) target=\(request.target) " +
                "fmt=\(request.format) bind=0x\(String(request.bind, radix: 16)) " +
                "\(request.width)x\(request.height)x\(request.depth) " +
                "array=\(request.arraySize) levels=\(request.lastLevel + 1) " +
                "samples=\(request.sampleCount) flags=0x\(String(request.flags, radix: 16))")
            var create = np_renderer_resource_3d(
                resource_id: request.resourceID,
                target: request.target,
                format: request.format,
                bind: request.bind,
                width: request.width,
                height: request.height,
                depth: request.depth,
                array_size: request.arraySize,
                last_level: request.lastLevel,
                nr_samples: request.sampleCount,
                flags: request.flags)
            guard np_renderer_resource_create_3d(venus, &create) == 0 else {
                respond(element, header.reply(.errOutOfMemory))
                return
            }
            do {
                let resource = try resources.adoptVirglResource(id: request.resourceID)
                publish(resource)
                notifyResourcePublished(request.resourceID)
            } catch {
                np_venus_unimport_blob(venus, request.resourceID)
                throw error
            }
            respond(element, header.reply(.okNoData))

        case .transferToHost3D, .transferFromHost3D:
            let request = try VirtioGPU.Transfer3D(parsing: &reader)
            let resource = try resources.require(request.resourceID)
            guard resource.isVirglResource else {
                respond(element, header.reply(.errInvalidResourceID))
                return
            }
            var box = np_renderer_box(
                x: request.box.x, y: request.box.y, z: request.box.z,
                width: request.box.width, height: request.box.height,
                depth: request.box.depth)
            let rc = np_renderer_transfer_3d(
                venus, request.resourceID, header.contextID, request.level,
                request.stride, request.layerStride, &box, request.offset,
                command == .transferFromHost3D)
            let direction = command == .transferFromHost3D ? "read" : "write"
            Self.note(
                "transfer3d \(direction) " +
                "res=\(request.resourceID) level=\(request.level) " +
                "box=\(request.box.x),\(request.box.y),\(request.box.z) " +
                "\(request.box.width)x\(request.box.height)x\(request.box.depth) " +
                "stride=\(request.stride) layer=\(request.layerStride) " +
                "offset=\(request.offset) rc=\(rc)")
            respond(element, header.reply(rc == 0 ? .okNoData : .errUnspecified))

        case .resourceAssignUUID:
            Self.log.error("unimplemented command \(String(describing: command), privacy: .public)")
            respond(element, header.reply(.errUnspecified))
        }
    }

    // MARK: - Guest backing

    private func makeGuestBacking(
        _ entries: [VirtioGPU.MemoryEntry], for resourceID: UInt32
    ) throws -> GuestBacking {
        guard let device else { throw BackingFailure.missingDevice }
        guard !entries.isEmpty else { throw BackingFailure.emptyBacking }
        guard guestBackings[resourceID] == nil else {
            throw BackingFailure.backingAlreadyAttached
        }

        var logicalOffset: UInt64 = 0
        var segments: [GuestBackingSegment] = []
        segments.reserveCapacity(entries.count)
        for entry in entries {
            guard entry.length > 0 else { throw BackingFailure.emptyBacking }
            let (end, overflow) = entry.address.addingReportingOverflow(UInt64(entry.length))
            guard !overflow, end >= entry.address,
                  let mapping = device.guestMemoryMapping(
                    atPhysicalAddress: entry.address, length: Int(entry.length))
            else {
                throw BackingFailure.guestMappingFailed(
                    address: entry.address, length: entry.length)
            }
            segments.append(GuestBackingSegment(
                logicalOffset: logicalOffset, mapping: mapping))
            let (next, countOverflow) = logicalOffset.addingReportingOverflow(UInt64(entry.length))
            guard !countOverflow else { throw BackingFailure.invalidBackingRange }
            logicalOffset = next
        }
        return GuestBacking(segments: segments, byteCount: logicalOffset)
    }

    // MARK: - Shared memory

    private func createBlobResource(
        _ request: VirtioGPU.ResourceCreateBlob,
        header: VirtioGPU.ControlHeader,
        element: VZVirtioQueueElement
    ) {
        if np_venus_is_live(venus) {
            adoptVenusBlob(request, header: header, element: element)
        } else {
            Self.note(
                "reject  create res=\(request.resourceID) blob_id=\(request.blobID) — no vkr mapping")
            respond(element, header.reply(.errOutOfMemory))
        }
    }

    private func mapIntoGuest(
        _ resource: GPUResource,
        atOffset offset: UInt64,
        element: VZVirtioQueueElement,
        header: VirtioGPU.ControlHeader
    ) throws {
        guard let region = hostVisibleRegion else {
            respond(element, header.reply(.errUnspecified))
            return
        }
        guard resource.isHostMappable else {
            Self.note("reject  map res=\(resource.resourceID) — DEVICE_LOCAL / Metal heap has no CPU pages")
            respond(element, header.reply(.errInvalidParameter))
            return
        }

        // The guest allocated this offset inside the region itself; the host only
        // honours it. virglrenderer supplies a host-page-aligned mapping, while
        // the offset comes from the guest's own allocator.
        let hostPage = UInt64(getpagesize())
        guard offset % hostPage == 0 else {
            Self.note("reject  map res=\(resource.resourceID) offset=\(offset) is not a multiple of the \(hostPage)-byte host page")
            Self.log.error(
                "resource \(resource.resourceID) offset \(offset) is not host-page aligned")
            respond(element, header.reply(.errInvalidParameter))
            return
        }

        enqueueRegionOperation { [weak self] done in
            guard let self, !rendererTornDown, let address = resource.baseAddress else {
                element.returnToQueue()
                done()
                return
            }
            region.mapMemory(
                address,
                atOffset: offset,
                size: UInt64(resource.byteCount)
            ) { error in
                guard !self.rendererTornDown else {
                    element.returnToQueue()
                    done()
                    return
                }
                if let error {
                    Self.log.error(
                        "mapping resource \(resource.resourceID) at +\(offset) failed: \(error.localizedDescription, privacy: .public)")
                    Self.note("reject  map res=\(resource.resourceID) offset=\(offset): \(error.localizedDescription)")
                    self.respond(element, header.reply(.errOutOfMemory))
                    done()
                    return
                }
                resource.mappedOffset = offset
                Self.log.info("resource \(resource.resourceID) mapped at region offset \(offset)")
                Self.note("map     res=\(resource.resourceID) offset=\(offset) size=\(resource.byteCount)")

                let info = VirtioGPU.MapInfoResponse(mapInfo: resource.mapInfo)
                self.respond(element, header.reply(.okMapInfo), body: info.encoded())
                done()
            }
        }
    }

    private func unmapFromGuest(
        _ resource: GPUResource,
        element: VZVirtioQueueElement?,
        header: VirtioGPU.ControlHeader?,
        completion: ((Bool) -> Void)? = nil
    ) {
        let finish = { [weak self] (success: Bool) in
            if let element, let header {
                if let self, !self.rendererTornDown {
                    self.respond(
                        element,
                        header.reply(success ? .okNoData : .errUnspecified))
                } else {
                    element.returnToQueue()
                }
            }
            completion?(success)
        }

        // UNMAP and UNREF are separate virtio commands and can both be queued
        // before VZ calls the asynchronous unmap completion. `mappedOffset` is
        // already nil by then, but the resource is not safe to release yet.
        // Make every later teardown action wait for the one real VZ unmap.
        if resource.unmapInFlight {
            resource.afterUnmap.append(finish)
            return
        }

        guard let offset = resource.mappedOffset else {
            finish(true)
            return
        }
        guard let region = hostVisibleRegion else {
            finish(false)
            return
        }

        // Claim the mapping synchronously. Tearing down a resource sends
        // RESOURCE_UNMAP_BLOB and RESOURCE_UNREF back to back, and the unmap
        // completion runs later, so leaving `mappedOffset` set until then would
        // let the unref unmap the same range a second time.
        resource.mappedOffset = nil
        resource.unmapInFlight = true

        // Read back before the mapping goes away: whatever the guest wrote is
        // still sitting in these pages.
        Self.note("unmap   res=\(resource.resourceID) guest left \(Self.peek(resource))")

        enqueueRegionOperation { [weak self] done in
            guard let self, !rendererTornDown else {
                resource.unmapInFlight = false
                let waiters = resource.afterUnmap
                resource.afterUnmap.removeAll(keepingCapacity: false)
                finish(false)
                waiters.forEach { $0(false) }
                done()
                return
            }
            region.unmapMemory(atOffset: offset, size: UInt64(resource.byteCount)) { error in
                if let error, !self.rendererTornDown {
                    Self.log.error("unmapping resource \(resource.resourceID) failed: \(error.localizedDescription, privacy: .public)")
                    Self.note("reject  unmap res=\(resource.resourceID) offset=\(offset): \(error.localizedDescription)")
                    // The API did not confirm that the mapping disappeared.
                    // Restoring the claimed offset keeps the backing reachable
                    // and lets a later reset retry instead of freeing live pages.
                    resource.mappedOffset = offset
                }
                resource.unmapInFlight = false
                let waiters = resource.afterUnmap
                resource.afterUnmap.removeAll(keepingCapacity: false)
                let success = error == nil
                finish(success)
                waiters.forEach { $0(success) }
                done()
            }
        }
    }

    // MARK: - Venus

    /// Common tail of RESOURCE_CREATE_BLOB once the backing exists.
    private func finishCreateBlob(
        _ resource: GPUResource,
        header: VirtioGPU.ControlHeader, element: VZVirtioQueueElement
    ) {
        Self.log.info(
            "resource \(resource.resourceID) created, \(resource.byteCount) bytes")
        publish(resource)
        Self.note("create  res=\(resource.resourceID) size=\(resource.byteCount) venus")
        notifyResourcePublished(resource.resourceID)
        respond(element, header.reply(.okNoData))
    }


    /// Mesa orders RESOURCE_CREATE_BLOB after vkAllocateMemory with
    /// vkWaitRingSeqnoMESA. A missing blob here is therefore a terminal
    /// renderer/protocol error, not an eventually-consistent state.
    private func adoptVenusBlob(
        _ request: VirtioGPU.ResourceCreateBlob,
        header: VirtioGPU.ControlHeader,
        element: VZVirtioQueueElement
    ) {
        var blob = np_venus_blob(
            resource_id: request.resourceID,
            blob_id: request.blobID,
            blob_flags: request.blobFlags,
            map_info: 0,
            pointer: nil,
            size: request.size)
        let createResult = np_venus_create_blob(venus, header.contextID, &blob)
        if createResult == 0 {
            do {
                let resource: GPUResource
                if let pointer = blob.pointer {
                    resource = try resources.adoptHostMapping(
                        id: request.resourceID, pointer: pointer,
                        size: blob.size, blobID: request.blobID,
                        mapInfo: blob.map_info)
                } else {
                    // DEVICE_LOCAL / MTLHeap: pixels stay in Metal; the guest
                    // only needs the resource id for linux-dmabuf present.
                    resource = try resources.adoptMetalHeapBlob(
                        id: request.resourceID, size: blob.size,
                        blobID: request.blobID)
                }
                finishCreateBlob(resource, header: header, element: element)
            } catch {
                np_venus_unimport_blob(venus, request.resourceID)
                Self.note(
                    "reject  create res=\(request.resourceID) blob_id=\(request.blobID): \(error.localizedDescription)")
                Self.log.error(
                    "could not adopt vkr mapping for res \(request.resourceID): \(error.localizedDescription, privacy: .public)")
                respond(element, header.reply(.errOutOfMemory))
            }
            return
        }
        Self.note(
            "reject  create res=\(request.resourceID) blob_id=\(request.blobID) rc=\(createResult) — renderer rejected ordered blob creation")
        let blobFlags = String(request.blobFlags, radix: 16)
        Self.log.error(
            "vkr rejected ordered blob creation for res \(request.resourceID), ctx \(header.contextID), blob_id \(request.blobID), flags 0x\(blobFlags, privacy: .public), size \(request.size), rc \(createResult)")
        respond(
            element,
            header.reply(createResult == -ENOMEM ? .errOutOfMemory : .errInvalidParameter))
    }

    /// Completes a fenced SUBMIT_3D. The C renderer may call back off-queue.
    private final class FenceWait {
        let element: VZVirtioQueueElement
        let header: VirtioGPU.ControlHeader
        let queue: DispatchQueue
        let finish: (VZVirtioQueueElement, VirtioGPU.ControlHeader, Bool) -> Void

        init(
            element: VZVirtioQueueElement, header: VirtioGPU.ControlHeader,
            queue: DispatchQueue,
            finish: @escaping (VZVirtioQueueElement, VirtioGPU.ControlHeader, Bool) -> Void
        ) {
            self.element = element
            self.header = header
            self.queue = queue
            self.finish = finish
        }
    }

    private func submitVenus(
        _ payload: [UInt8],
        header: VirtioGPU.ControlHeader,
        element: VZVirtioQueueElement
    ) {
        // A compositor never submits. Mesa Venus does, and only after GET_CAPSET
        // advertised a real renderer. If that renderer is not live we must not
        // say OK — the guest would believe the GPU ran.
        let wait = FenceWait(
            element: element, header: header, queue: deviceQueue
        ) { [weak self] element, header, ok in
            if header.wantsFence {
                Self.note(
                    "fence   ctx=\(header.contextID) ring=\(header.ringIndex) id=\(header.fenceID) ok=\(ok)")
            }
            self?.respond(element, header.reply(ok ? .okNoData : .errUnspecified))
        }
        let retained = Unmanaged.passRetained(wait)
        let rc = payload.withUnsafeBytes { raw -> Int32 in
            np_venus_submit(
                venus, header.contextID, UInt32(header.ringIndex), raw.baseAddress,
                UInt32(payload.count), header.wantsFence, header.fenceID,
                { user, _, ok in
                    guard let user else { return }
                    let wait = Unmanaged<FenceWait>.fromOpaque(user).takeRetainedValue()
                    wait.queue.async { wait.finish(wait.element, wait.header, ok) }
                },
                retained.toOpaque())
        }
        if rc != 0 {
            Self.note("submit3d ctx=\(header.contextID) \(payload.count) bytes refused rc=\(rc)")
        } else {
            Self.note(
                "submit3d ctx=\(header.contextID) ring=\(header.ringIndex) \(payload.count) bytes fence=\(header.wantsFence)")
        }
    }

    private func notifyResourcePublished(_ resourceID: UInt32) {
        guard let onResourcePublished else { return }
        DispatchQueue.main.async { [weak self] in
            // Metal-heap blobs have no CPU mapping; WindowBridge asks for
            // the MTLTexture with the frame's width/height at commit time.
            guard self?.buffer(forResource: resourceID) != nil else { return }
            onResourcePublished(resourceID)
        }
    }

    // MARK: - Responses

    private func respond(
        _ element: VZVirtioQueueElement,
        _ header: VirtioGPU.ControlHeader,
        body: Data = Data()
    ) {
        var response = header.encoded()
        response.append(body)
        do {
            try element.write(response)
        } catch {
            Self.log.error("could not write response: \(error.localizedDescription, privacy: .public)")
        }
        element.returnToQueue()
    }
}

// MARK: - Virtualization delegates

@available(macOS 27.0, *)
extension VirtioGPUDevice: VZCustomVirtioDeviceConfigurationDelegate {
    public func customVirtioConfiguration(
        _ deviceConfiguration: VZCustomVirtioDeviceConfiguration,
        didCreateDevice device: VZCustomVirtioDevice
    ) {
        // Both `delegate` properties in this API are weak; the strong reference
        // chain runs configuration -> provider -> (weak) self, so the VM owner
        // must keep this object alive.
        device.delegate = self
        self.device = device
        hostVisibleRegion = device.sharedMemoryRegions.first {
            $0.regionID == VirtioGPU.hostVisibleRegionID
        }
        Self.log.info(
            "device created with \(device.sharedMemoryRegions.count) shared memory region(s)")
    }
}

@available(macOS 27.0, *)
extension VirtioGPUDevice: VZCustomVirtioDeviceDelegate {
    public func customVirtioDevice(
        _ device: VZCustomVirtioDevice,
        didReceiveNotificationFor queue: VZVirtioQueue
    ) {
        while let element = queue.nextElement() {
            if rendererTornDown {
                // Every taken element must be returned, or Virtualization
                // raises from VZVirtioQueueElement.dealloc.
                element.returnToQueue()
                continue
            }
            handle(element, queueIndex: queue.queueIndex)
        }
    }

    public func customVirtioDeviceDidAcceptDriverOk(_ device: VZCustomVirtioDevice) {
        let negotiated = device.negotiatedFeatures
        let blob = (negotiated?.subset0 ?? 0) & (1 << VirtioGPU.Feature.resourceBlob) != 0
        let contextInit = (negotiated?.subset0 ?? 0) & (1 << VirtioGPU.Feature.contextInit) != 0
        Self.log.info(
            "guest driver bound: RESOURCE_BLOB=\(blob), CONTEXT_INIT=\(contextInit), subset0=0x\(String(negotiated?.subset0 ?? 0, radix: 16))")
        onDriverReady?(negotiated)
    }

    public func customVirtioDeviceWillStop(_ device: VZCustomVirtioDevice) {
        // Final teardown: drop guest state and destroy Venus/virglrenderer on
        // this queue so AppKit's main actor never thrd_joins the render thread.
        shutdownRenderer()
    }

    public func customVirtioDeviceWillPause(_ device: VZCustomVirtioDevice) {}
    public func customVirtioDeviceWillResume(_ device: VZCustomVirtioDevice) {}

    public func customVirtioDeviceWillReset(_ device: VZCustomVirtioDevice) {
        // Reset clears guest contexts/resources but keeps virglrenderer alive —
        // Virtualization can reset during attach, long before willStop.
        clearGuestRendererState()
    }

    /// Drop mappings and Venus contexts; leave the host renderer running.
    private func clearGuestRendererState(completion: @escaping () -> Void = {}) {
		cancelDeferredBlobCreates()
        let activeResources = resources.all
        let cleanupResources = activeResources + Array(retiringResources.values)
        let contextIDs = Array(contexts)
        for resource in cleanupResources
        where resource.mappedOffset != nil || resource.unmapInFlight {
            unmapFromGuest(resource, element: nil, header: nil)
        }
        // This operation is queued after every asynchronous aperture unmap.
        // Renderer mappings must remain alive until the guest mapping is gone.
        enqueueRegionOperation { [self] done in
            for resource in cleanupResources {
                // A RESOURCE_UNREF already in flight may have completed and
                // unimported this retiring object before the reset barrier ran.
                // Only the table which still owns the object may retire it.
                guard resources[resource.resourceID] === resource ||
                        retiringResources[resource.resourceID] === resource
                else { continue }
                if resource.mappedOffset == nil && !resource.unmapInFlight {
                    np_venus_unimport_blob(venus, resource.resourceID)
                    retiringResources.removeValue(forKey: resource.resourceID)
                    quarantinedResourceIDs.remove(resource.resourceID)
                } else {
                    retiringResources[resource.resourceID] = resource
                    quarantinedResourceIDs.insert(resource.resourceID)
                    Self.log.error(
                        "resource \(resource.resourceID) remains mapped after reset; quarantined")
                }
            }
            for ctxID in contextIDs { np_venus_context_destroy(venus, ctxID) }
            // virglrenderer has now detached every iovec, so its guest-memory
            // mappings can finally be released.
            guestBackings.removeAll(keepingCapacity: true)
            contexts.removeAll(keepingCapacity: true)
            resources.removeAll()
            publishedLock.lock()
            published.removeAll(keepingCapacity: true)
            metalTextures.removeAll(keepingCapacity: true)
            publishedLock.unlock()
            completion()
            done()
        }
    }

    /// Full host-renderer teardown. Safe to call more than once.
    private func shutdownRenderer() {
        guard !rendererTornDown else { return }
        Self.note("tearing down Venus / virglrenderer")
        rendererTornDown = true
		cancelDeferredBlobCreates()

        // `willStop` is the ownership boundary for the whole custom device.
        // Waiting for individual VZ shared-region unmaps here can deadlock:
        // Virtualization is allowed to stop delivering their asynchronous
        // completions once this callback returns. The guest can no longer
        // access the device, and VZ tears down the complete region itself, so
        // release renderer objects synchronously and let any late completion
        // take the guarded path above.
        let cleanupResources = resources.all + Array(retiringResources.values)
        let contextIDs = Array(contexts)
        for resource in cleanupResources {
            np_venus_unimport_blob(venus, resource.resourceID)
        }
        guestBackings.removeAll(keepingCapacity: false)
        for ctxID in contextIDs { np_venus_context_destroy(venus, ctxID) }
        contexts.removeAll()
        resources.removeAll()
        retiringResources.removeAll(keepingCapacity: false)
        quarantinedResourceIDs.removeAll(keepingCapacity: false)
        publishedLock.lock()
        published.removeAll(keepingCapacity: false)
        metalTextures.removeAll(keepingCapacity: false)
        publishedLock.unlock()
        np_venus_destroy(venus)
        rendererDestroyed = true
    }
}
