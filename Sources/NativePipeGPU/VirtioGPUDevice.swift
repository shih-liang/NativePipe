import Darwin
import Foundation
import IOSurface
import NativePipeVenus
import Virtualization
import os

/// Host virtio-gpu. Guest: stock `virtio_gpu.ko` + Mesa Venus + the
/// NativePipe compositor. One instance can expose both a Venus render node and
/// an optional KMS scanout; no Apple graphics device is required in that mode.
///
///   * CREATE_BLOB from Mesa Venus → ordinary renderer allocations. Wayland
///     linux-dmabuf commits continue naming those original client textures.
///   * The host retains each scene source while Metal composites it directly
///     into a CAMetalDrawable, then releases the source on GPU completion.
///   * IOSurface-backed resources remain only for the optional legacy 2D
///     framebuffer path.
///   * SUBMIT_3D → virglrenderer (vkr) → MoltenVK. Venus is not here.
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
    private var contexts: [UInt32: GuestContext] = [:]
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
    private var latestScanout: ScanoutFrame?
    private var scanoutObservers: [UUID: (ScanoutFrame?) -> Void] = [:]
    private var scanoutDeliveryScheduled = false

    public struct ScanoutConfiguration: Equatable, Sendable {
        public let width: UInt32
        public let height: UInt32

        public init(width: UInt32, height: UInt32) {
            self.width = width
            self.height = height
        }
    }

    /// Immutable description of the resource currently bound to scanout 0.
    /// Raw blob access is scanout plumbing only; application windows resolve
    /// the compositor scene through `gpuMetalTexture(forResource:...)`.
    public struct ScanoutFrame: Equatable, Sendable {
        public let resourceID: UInt32
        public let rectangle: VirtioGPU.Rect
        public let resourceWidth: Int
        public let resourceHeight: Int
        public let bytesPerRow: Int
        public let format: UInt32
        public let planeOffset: Int
        public let serial: UInt64
    }

    /// Published host view of one virtio-gpu resource. `surface` belongs only
    /// to the legacy 2D framebuffer path; application windows request the
    /// compositor's Metal texture by resource id.
    public struct PublishedBuffer {
        public var surface: IOSurfaceRef?
        public var pointer: UnsafeMutableRawPointer?
        public var byteCount: Int
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

    /// Additive observer used by the inline preview and the detached display
    /// window. Callbacks are always delivered on the main queue and immediately
    /// replay the current frame (including nil before the first scanout).
    /// FLUSH bursts are latest-wins: at most one delivery is queued on main.
    @discardableResult
    public func observeScanout(_ observer: @escaping (ScanoutFrame?) -> Void) -> UUID {
        let token = UUID()
        publishedLock.lock()
        scanoutObservers[token] = observer
        let shouldSchedule = !scanoutDeliveryScheduled
        if shouldSchedule { scanoutDeliveryScheduled = true }
        publishedLock.unlock()
        if shouldSchedule { scheduleScanoutDelivery() }
        return token
    }

    public func removeScanoutObserver(_ token: UUID) {
        publishedLock.lock()
        scanoutObservers[token] = nil
        publishedLock.unlock()
    }

    /// The IOSurface behind a legacy 2D framebuffer resource, or nil.
    public func surface(forResource resourceID: UInt32) -> IOSurfaceRef? {
        buffer(forResource: resourceID)?.surface
    }

    /// Whether this id currently names a live host resource. Unknown ids are
    /// treated as the CREATE_BLOB/window-channel ordering case by WindowBridge.
    public func isResourcePublished(_ resourceID: UInt32) -> Bool {
        publishedLock.lock()
        defer { publishedLock.unlock() }
        return published[resourceID] != nil
    }

    /// Mapping of a Venus resource for the optional full-VM scanout path.
    /// Application windows never call this API.
    public func gpuMemory(forResource resourceID: UInt32) -> (UnsafeMutableRawPointer, Int)? {
        guard let published = buffer(forResource: resourceID), published.surface == nil,
              published.pointer != nil else {
            return nil
        }
        return (published.pointer!, published.byteCount)
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
              entry.byteCount >= requiredBytes,
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
            surface: resource.surface,
            pointer: resource.baseAddress,
            byteCount: resource.byteCount)
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

    private struct GuestContext {
        var capsetID: UInt32
        var debugName: String
        var resources: Set<UInt32> = []
    }

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

    private struct ScanoutBinding {
        let resourceID: UInt32
        let rectangle: VirtioGPU.Rect
        let resourceWidth: Int
        let resourceHeight: Int
        let bytesPerRow: Int
        let format: UInt32
        let planeOffset: Int
    }

    private enum ScanoutFailure: LocalizedError {
        case missingDevice
        case emptyBacking
        case invalidBackingRange
        case guestMappingFailed(address: UInt64, length: UInt32)
        case invalidTransfer

        var errorDescription: String? {
            switch self {
            case .missingDevice: return "custom virtio device is not ready"
            case .emptyBacking: return "resource backing is empty"
            case .invalidBackingRange: return "resource backing range overflow"
            case .guestMappingFailed(let address, let length):
                return "could not map guest range 0x\(String(address, radix: 16))+\(length)"
            case .invalidTransfer: return "2D transfer is outside the resource or backing"
            }
        }
    }

    private var guestBackings: [UInt32: GuestBacking] = [:]
    private var scanoutBinding: ScanoutBinding?
    private var scanoutSerial: UInt64 = 0

    /// Address space only, so the default is generous rather than frugal.
    public static let defaultApertureSize: UInt64 = 4 << 30

    /// Size of the host-visible aperture this device advertises.
    public let apertureSize: UInt64
    public let scanoutConfiguration: ScanoutConfiguration?

    public init(
        hostVisibleApertureSize: UInt64 = VirtioGPUDevice.defaultApertureSize,
        scanout: ScanoutConfiguration? = nil
    ) {
        apertureSize = hostVisibleApertureSize
        scanoutConfiguration = scanout
        deviceQueue = DispatchQueue(label: "com.nativepipe.gpu.device", qos: .userInteractive)

        let configuration = VZCustomVirtioDeviceConfiguration()
        configuration.deviceID = VirtioGPU.deviceID
        configuration.pciClassID = VirtioGPU.pciClassDisplay
        configuration.pciSubclassID = VirtioGPU.pciSubclassOther
        configuration.virtioQueueCount = VirtioGPU.queueCount

        // Offered but not demanded. Making these mandatory would refuse to start
        // the device against a guest kernel too old to accept them; offering them
        // instead lets any virtio_gpu driver bind, and the negotiated set tells
        // us afterwards what we actually got.
        configuration.optionalFeatures.subset0 = VirtioGPU.Feature.mask([
            // Required for the 3D ioctls even though venus, not virgl, is the
            // renderer; see the note on Feature.virgl.
            VirtioGPU.Feature.virgl,
            VirtioGPU.Feature.resourceBlob,
            VirtioGPU.Feature.contextInit,
        ])

        // Render-only VMs keep zero scanouts. Framebuffer-enabled VMs expose a
        // single KMS head from this same device rather than attaching Apple's
        // second, unrelated virtio-gpu device.
        let deviceConfig = VirtioGPU.DeviceConfig(
            numScanouts: scanout == nil ? 0 : 1, numCapsets: 1)
        configuration.deviceSpecificConfiguration =
            VZVirtioDeviceSpecificConfiguration(configurationData: deviceConfig.encoded())

        configuration.sharedMemoryRegions = [
            VZVirtioSharedMemoryRegionConfiguration(
                regionID: VirtioGPU.hostVisibleRegionID, size: hostVisibleApertureSize)
        ]

        self.configuration = configuration
        guard let created = np_venus_create() else {
            fatalError("Venus host failed to allocate")
        }
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
            let display = scanoutConfiguration
            let info = VirtioGPU.DisplayInfoResponse(
                width: display?.width ?? 0,
                height: display?.height ?? 0,
                enabled: display != nil)
            respond(element, header.reply(.okDisplayInfo), body: info.encoded())

        case .resourceCreate2D:
            let request = try VirtioGPU.ResourceCreate2D(parsing: &reader)
            guard retiringResources[request.resourceID] == nil else {
                respond(element, header.reply(.errInvalidResourceID))
                return
            }
            do {
                let resource = try resources.create2D(
                    id: request.resourceID, format: request.format,
                    width: request.width, height: request.height)
                publish(resource)
                Self.log.info(
                    "2D resource \(request.resourceID) created: \(request.width)x\(request.height), format \(request.format)")
                Self.note(
                    "create2d res=\(request.resourceID) \(request.width)x\(request.height) fmt=\(request.format)")
                respond(element, header.reply(.okNoData))
            } catch ResourceAllocationError.allocationFailed,
                    ResourceAllocationError.tooSmall {
                respond(element, header.reply(.errOutOfMemory))
            }

        case .resourceAttachBacking:
            let request = try VirtioGPU.ResourceAttachBacking(parsing: &reader)
            _ = try resources.require(request.resourceID)
            try attachGuestBacking(request.entries, to: request.resourceID)
            respond(element, header.reply(.okNoData))

        case .resourceDetachBacking:
            let request = try VirtioGPU.ResourceDetachBacking(parsing: &reader)
            _ = try resources.require(request.resourceID)
            guestBackings.removeValue(forKey: request.resourceID)
            respond(element, header.reply(.okNoData))

        case .transferToHost2D:
            let request = try VirtioGPU.TransferToHost2D(parsing: &reader)
            try transferToHost2D(request)
            respond(element, header.reply(.okNoData))

        case .setScanout:
            let request = try VirtioGPU.SetScanout(parsing: &reader)
            guard scanoutConfiguration != nil, request.scanoutID == 0 else {
                respond(element, header.reply(.errInvalidScanoutID))
                return
            }
            if request.resourceID == 0 {
                setScanout(nil)
            } else {
                let resource = try resources.require(request.resourceID)
                guard let metadata = resource.twoDimensional,
                      request.rectangle.fits(
                        width: UInt32(metadata.width), height: UInt32(metadata.height))
                else {
                    respond(element, header.reply(.errInvalidParameter))
                    return
                }
                setScanout(ScanoutBinding(
                    resourceID: request.resourceID,
                    rectangle: request.rectangle,
                    resourceWidth: metadata.width,
                    resourceHeight: metadata.height,
                    bytesPerRow: resource.geometry?.bytesPerRow
                        ?? metadata.sourceBytesPerRow,
                    format: metadata.format,
                    planeOffset: 0))
            }
            respond(element, header.reply(.okNoData))

        case .setScanoutBlob:
            let request = try VirtioGPU.SetScanoutBlob(parsing: &reader)
            guard scanoutConfiguration != nil, request.scanoutID == 0 else {
                respond(element, header.reply(.errInvalidScanoutID))
                return
            }
            if request.resourceID == 0 {
                setScanout(nil)
                respond(element, header.reply(.okNoData))
                return
            }
            let resource = try resources.require(request.resourceID)
            let (minimumStride, strideOverflow) = request.width.multipliedReportingOverflow(by: 4)
            // Set-scanout-blob describes one linear 32-bit plane here. Reject
            // geometry that would read beyond the resource before publishing it
            // to AppKit; the view should never be the protocol validator.
            let imageBytes = UInt64(request.strides[0]) * UInt64(request.height)
            let requiredBytes = UInt64(request.offsets[0]) + imageBytes
            guard !strideOverflow,
                  let format = VirtioGPU.Format(rawValue: request.format),
                  format.isSupportedScanout32Bit,
                  request.width > 0, request.height > 0,
                  request.rectangle.fits(width: request.width, height: request.height),
                  let stride = request.strides.first, stride >= minimumStride,
                  let offset = request.offsets.first,
                  Int(exactly: stride) != nil, Int(exactly: offset) != nil,
                  requiredBytes <= UInt64(resource.byteCount)
            else {
                respond(element, header.reply(.errInvalidParameter))
                return
            }
            setScanout(ScanoutBinding(
                resourceID: request.resourceID,
                rectangle: request.rectangle,
                resourceWidth: Int(request.width),
                resourceHeight: Int(request.height),
                bytesPerRow: Int(stride),
                format: request.format,
                planeOffset: Int(offset)))
            respond(element, header.reply(.okNoData))

        case .resourceFlush:
            let request = try VirtioGPU.ResourceFlush(parsing: &reader)
            _ = try resources.require(request.resourceID)
            if scanoutBinding?.resourceID == request.resourceID {
                publishCurrentScanout()
            }
            respond(element, header.reply(.okNoData))

        case .getCapsetInfo:
            let request = try VirtioGPU.GetCapsetInfo(parsing: &reader)
            // Only one capset is advertised, at index 0.
            guard request.capsetIndex == 0 else {
                respond(element, header.reply(.errInvalidParameter))
                return
            }
            var maxVersion: UInt32 = 0
            var maxSize: UInt32 = 0
            np_venus_capset_info(venus, &maxVersion, &maxSize)
            let info = VirtioGPU.CapsetInfoResponse(
                capsetID: VirtioGPU.Capset.venus.rawValue,
                capsetMaxVersion: maxVersion,
                capsetMaxSize: maxSize)
            respond(element, header.reply(.okCapsetInfo), body: info.encoded())

        case .getCapset:
            let request = try VirtioGPU.GetCapset(parsing: &reader)
            var maxVersion: UInt32 = 0
            var maxSize: UInt32 = 0
            np_venus_capset_info(venus, &maxVersion, &maxSize)
            guard request.capsetID == VirtioGPU.Capset.venus.rawValue, maxSize > 0 else {
                respond(element, header.reply(.okCapset))
                return
            }
            var blob = Data(count: Int(maxSize))
            let written = blob.withUnsafeMutableBytes { raw -> UInt32 in
                np_venus_fill_caps(venus, request.capsetVersion, raw.baseAddress, maxSize)
            }
            respond(element, header.reply(.okCapset), body: blob.prefix(Int(written)))

        case .ctxCreate:
            let request = try VirtioGPU.ContextCreate(parsing: &reader)
            contexts[header.contextID] = GuestContext(
                capsetID: request.capsetID, debugName: request.debugName)
            let rc = np_venus_context_create(
                venus, header.contextID, request.capsetID, request.debugName)
            guard rc == 0 else {
                contexts.removeValue(forKey: header.contextID)
                respond(element, header.reply(.errInvalidParameter))
                return
            }
            Self.log.info(
                "context \(header.contextID) created for capset \(request.capsetID) (\(request.debugName, privacy: .public))")
            respond(element, header.reply(.okNoData))

        case .ctxDestroy:
            np_venus_context_destroy(venus, header.contextID)
            contexts.removeValue(forKey: header.contextID)
            respond(element, header.reply(.okNoData))

        case .ctxAttachResource:
            let request = try VirtioGPU.ContextResource(parsing: &reader)
            let resource = try resources.require(request.resourceID)
            resource.attachedContexts.insert(header.contextID)
            contexts[header.contextID]?.resources.insert(request.resourceID)
            _ = np_venus_attach(venus, header.contextID, request.resourceID)
            respond(element, header.reply(.okNoData))

        case .ctxDetachResource:
            let request = try VirtioGPU.ContextResource(parsing: &reader)
            resources[request.resourceID]?.attachedContexts.remove(header.contextID)
            contexts[header.contextID]?.resources.remove(request.resourceID)
            _ = np_venus_detach(venus, header.contextID, request.resourceID)
            respond(element, header.reply(.okNoData))

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
            guestBackings.removeValue(forKey: request.resourceID)
            if scanoutBinding?.resourceID == request.resourceID { setScanout(nil) }
            guard let resource = resources.remove(request.resourceID) else {
                respond(element, header.reply(.okNoData))
                return
            }
            if resource.twoDimensional != nil {
                respond(element, header.reply(.okNoData))
                return
            }
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

        case .resourceCreate3D, .transferToHost3D, .transferFromHost3D,
             .resourceAssignUUID:
            Self.log.error("unimplemented command \(String(describing: command), privacy: .public)")
            respond(element, header.reply(.errUnspecified))
        }
    }

    // MARK: - 2D scanout

    private func attachGuestBacking(
        _ entries: [VirtioGPU.MemoryEntry], to resourceID: UInt32
    ) throws {
        guard let device else { throw ScanoutFailure.missingDevice }
        guard !entries.isEmpty else { throw ScanoutFailure.emptyBacking }

        var logicalOffset: UInt64 = 0
        var segments: [GuestBackingSegment] = []
        segments.reserveCapacity(entries.count)
        for entry in entries {
            guard entry.length > 0 else { throw ScanoutFailure.emptyBacking }
            let (end, overflow) = entry.address.addingReportingOverflow(UInt64(entry.length))
            guard !overflow, end >= entry.address,
                  let mapping = device.guestMemoryMapping(
                    atPhysicalAddress: entry.address, length: Int(entry.length))
            else {
                throw ScanoutFailure.guestMappingFailed(
                    address: entry.address, length: entry.length)
            }
            segments.append(GuestBackingSegment(
                logicalOffset: logicalOffset, mapping: mapping))
            let (next, countOverflow) = logicalOffset.addingReportingOverflow(UInt64(entry.length))
            guard !countOverflow else { throw ScanoutFailure.invalidBackingRange }
            logicalOffset = next
        }
        guestBackings[resourceID] = GuestBacking(
            segments: segments, byteCount: logicalOffset)
        Self.note("backing res=\(resourceID) entries=\(entries.count) bytes=\(logicalOffset)")
    }

    private func transferToHost2D(_ request: VirtioGPU.TransferToHost2D) throws {
        let resource = try resources.require(request.resourceID)
        guard let metadata = resource.twoDimensional,
              let surface = resource.surface,
              let backing = guestBackings[request.resourceID],
              request.rectangle.fits(
                width: UInt32(metadata.width), height: UInt32(metadata.height))
        else { throw ScanoutFailure.invalidTransfer }

        let x = Int(request.rectangle.x)
        let y = Int(request.rectangle.y)
        let width = Int(request.rectangle.width)
        let height = Int(request.rectangle.height)
        let rowBytes = width * 4
        let destinationStride = IOSurfaceGetBytesPerRow(surface)
        guard destinationStride >= metadata.width * 4,
              let destination = resource.baseAddress
        else { throw ScanoutFailure.invalidTransfer }

        IOSurfaceLock(surface, [], nil)
        defer { IOSurfaceUnlock(surface, [], nil) }

        if x == 0, width == metadata.width,
           destinationStride == metadata.sourceBytesPerRow {
            let (copyBytes, copyOverflow) = rowBytes.multipliedReportingOverflow(by: height)
            let (destinationOffset, offsetOverflow) = y.multipliedReportingOverflow(
                by: destinationStride)
            let (destinationEnd, endOverflow) = destinationOffset.addingReportingOverflow(
                copyBytes)
            guard !copyOverflow, !offsetOverflow, !endOverflow,
                  destinationEnd <= resource.byteCount
            else { throw ScanoutFailure.invalidTransfer }
            try copyGuestBytes(
                from: backing, offset: request.offset,
                to: destination.advanced(by: destinationOffset), count: copyBytes)
            return
        }

        for row in 0..<height {
            let (rowDelta, rowOverflow) = UInt64(row).multipliedReportingOverflow(
                by: UInt64(metadata.sourceBytesPerRow))
            let (sourceOffset, offsetOverflow) = request.offset.addingReportingOverflow(rowDelta)
            let (destinationRow, rowIndexOverflow) = y.addingReportingOverflow(row)
            let (destinationRowOffset, destinationRowOverflow) =
                destinationRow.multipliedReportingOverflow(by: destinationStride)
            let (destinationColumnOffset, destinationColumnOverflow) =
                x.multipliedReportingOverflow(by: 4)
            let (destinationOffset, destinationOffsetOverflow) =
                destinationRowOffset.addingReportingOverflow(destinationColumnOffset)
            let (destinationEnd, destinationEndOverflow) =
                destinationOffset.addingReportingOverflow(rowBytes)
            guard !rowOverflow, !offsetOverflow,
                  !rowIndexOverflow, !destinationRowOverflow,
                  !destinationColumnOverflow, !destinationOffsetOverflow,
                  !destinationEndOverflow, destinationOffset >= 0,
                  destinationEnd <= resource.byteCount
            else { throw ScanoutFailure.invalidTransfer }
            try copyGuestBytes(
                from: backing, offset: sourceOffset,
                to: destination.advanced(by: destinationOffset), count: rowBytes)
        }
        Self.note(
            "transfer2d res=\(request.resourceID) rect=\(x),\(y) \(width)x\(height) offset=\(request.offset)")
    }

    private func copyGuestBytes(
        from backing: GuestBacking, offset: UInt64,
        to destination: UnsafeMutableRawPointer, count: Int
    ) throws {
        guard count >= 0 else { throw ScanoutFailure.invalidTransfer }
        let (end, overflow) = offset.addingReportingOverflow(UInt64(count))
        guard !overflow, end <= backing.byteCount else {
            throw ScanoutFailure.invalidTransfer
        }

        var cursor = offset
        var copied = 0
        guard var segmentIndex = segmentIndex(containing: cursor, in: backing.segments)
        else { throw ScanoutFailure.invalidTransfer }
        while copied < count {
            guard segmentIndex < backing.segments.count else {
                throw ScanoutFailure.invalidTransfer
            }
            let segment = backing.segments[segmentIndex]
            let within = cursor - segment.logicalOffset
            guard within < UInt64(segment.mapping.length) else {
                throw ScanoutFailure.invalidTransfer
            }
            let available = UInt64(segment.mapping.length) - within
            let chunk = min(count - copied, Int(available))
            guard chunk > 0 else { throw ScanoutFailure.invalidTransfer }
            memcpy(
                destination.advanced(by: copied),
                segment.mapping.mutableBytes.advanced(by: Int(within)),
                chunk)
            copied += chunk
            cursor += UInt64(chunk)
            if copied < count { segmentIndex += 1 }
        }
    }

    /// Guest backing lists regularly contain hundreds of pages. Damage copies
    /// call this once per row, so a linear search here turns pointer motion and
    /// console scrolling into O(rows * pages). Locate the first page in O(log n)
    /// and then advance sequentially across page boundaries.
    private func segmentIndex(
        containing offset: UInt64, in segments: [GuestBackingSegment]
    ) -> Int? {
        var lower = 0
        var upper = segments.count
        while lower < upper {
            let middle = lower + (upper - lower) / 2
            if segments[middle].logicalOffset <= offset {
                lower = middle + 1
            } else {
                upper = middle
            }
        }
        guard lower > 0 else { return nil }
        let index = lower - 1
        let segment = segments[index]
        let within = offset - segment.logicalOffset
        return within < UInt64(segment.mapping.length) ? index : nil
    }

    private func setScanout(_ binding: ScanoutBinding?) {
        let previous = scanoutBinding
        scanoutBinding = binding
        if let binding,
           previous?.resourceID != binding.resourceID
            || previous?.rectangle != binding.rectangle {
            Self.log.info(
                "scanout 0 bound to resource \(binding.resourceID), rect \(binding.rectangle.x),\(binding.rectangle.y) \(binding.rectangle.width)x\(binding.rectangle.height)")
        } else if binding == nil, previous != nil {
            Self.log.info("scanout 0 disabled")
        }
        publishCurrentScanout()
    }

    private func publishCurrentScanout() {
        let frame: ScanoutFrame?
        if let binding = scanoutBinding {
            scanoutSerial &+= 1
            frame = ScanoutFrame(
                resourceID: binding.resourceID,
                rectangle: binding.rectangle,
                resourceWidth: binding.resourceWidth,
                resourceHeight: binding.resourceHeight,
                bytesPerRow: binding.bytesPerRow,
                format: binding.format,
                planeOffset: binding.planeOffset,
                serial: scanoutSerial)
        } else {
            frame = nil
        }

        publishedLock.lock()
        latestScanout = frame
        let shouldSchedule = !scanoutObservers.isEmpty && !scanoutDeliveryScheduled
        if shouldSchedule { scanoutDeliveryScheduled = true }
        publishedLock.unlock()
        if shouldSchedule { scheduleScanoutDelivery() }
    }

    private func scheduleScanoutDelivery() {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.publishedLock.lock()
            let frame = self.latestScanout
            let observers = Array(self.scanoutObservers.values)
            self.scanoutDeliveryScheduled = false
            self.publishedLock.unlock()
            for observer in observers { observer(frame) }
        }
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
        // honours it. The pointer and length are host-page aligned because they
        // come from an IOSurface, but the offset comes from the guest's own
        // allocator, which works in guest pages — a quarter the size here.
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

                let info = VirtioGPU.MapInfoResponse(mapInfo: .cached)
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
            pointer: nil,
            size: request.size)
        let createResult = np_venus_create_blob(venus, header.contextID, &blob)
        if createResult == 0 {
            do {
                let resource: GPUResource
                if let pointer = blob.pointer {
                    resource = try resources.adoptHostMapping(
                        id: request.resourceID, pointer: pointer,
                        size: blob.size, blobID: request.blobID)
                } else {
                    // DEVICE_LOCAL / MTLHeap: pixels stay in Metal; the guest
                    // only needs the resource id for linux-dmabuf present.
                    resource = try resources.adoptMetalHeapBlob(
                        id: request.resourceID, size: blob.size,
                        blobID: request.blobID)
                }
                finishCreateBlob(resource, header: header, element: element)
            } catch {
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
        Self.log.error(
            "vkr rejected ordered blob creation for res \(request.resourceID), blob_id \(request.blobID), rc \(createResult)")
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
        setScanout(nil)
        guestBackings.removeAll(keepingCapacity: true)
        let activeResources = resources.all
        let cleanupResources = activeResources + Array(retiringResources.values)
        let contextIDs = Array(contexts.keys)
        for resource in cleanupResources
        where resource.mappedOffset != nil || resource.unmapInFlight {
            unmapFromGuest(resource, element: nil, header: nil)
        }
        // This operation is queued after every asynchronous aperture unmap.
        // Renderer mappings must remain alive until the guest mapping is gone.
        enqueueRegionOperation { [self] done in
            for resource in cleanupResources where resource.twoDimensional == nil {
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
            contexts.removeAll()
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
        setScanout(nil)
        guestBackings.removeAll(keepingCapacity: false)

        // `willStop` is the ownership boundary for the whole custom device.
        // Waiting for individual VZ shared-region unmaps here can deadlock:
        // Virtualization is allowed to stop delivering their asynchronous
        // completions once this callback returns. The guest can no longer
        // access the device, and VZ tears down the complete region itself, so
        // release renderer objects synchronously and let any late completion
        // take the guarded path above.
        let cleanupResources = resources.all + Array(retiringResources.values)
        let contextIDs = Array(contexts.keys)
        for resource in cleanupResources where resource.twoDimensional == nil {
            np_venus_unimport_blob(venus, resource.resourceID)
        }
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
