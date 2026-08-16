import Foundation
import IOSurface
import NativePipeVenus
import Virtualization
import os

/// Host virtio-gpu. Guest: stock `virtio_gpu.ko` + Mesa Venus + the
/// NativePipe compositor. This type is only the host device.
///
///   * CREATE_BLOB from the compositor → IOSurface, mapped into the guest,
///     shown by `CALayer.contents` (CPU windows).
///   * CREATE_BLOB from Mesa Venus → adopt vkr's existing mapping. The
///     window presents that mapping on a CAMetalLayer (GPU windows).
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
    static let hostProbePattern: UInt32 = 0xA5A5_A5A5

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
    /// RESOURCE_UNREF can overtake the window-channel commit that names the
    /// resource: virtio-gpu and vsock are independent queues. Keep a small LRU
    /// so AppKit can still resolve an already-sent frame.
    private var retiredPublished: [UInt32: PublishedBuffer] = [:]
    private var retiredPublishedOrder: [UInt32] = []
    private static let retiredPublishedLimit = 16

    /// Host view of one virtio-gpu resource, for the window to present.
    public struct PublishedBuffer {
        public var surface: IOSurfaceRef?
        public var pointer: UnsafeMutableRawPointer?
        public var byteCount: Int
    }

    /// The IOSurface behind a CPU/compositor resource, or nil.
    public func surface(forResource resourceID: UInt32) -> IOSurfaceRef? {
        buffer(forResource: resourceID)?.surface
    }

    /// Mapping of a Venus resource that already lives in MoltenVK. The
    /// window presents this through a CAMetalLayer; nothing is copied.
    public func gpuMemory(forResource resourceID: UInt32) -> (UnsafeMutableRawPointer, Int)? {
        guard let published = buffer(forResource: resourceID), published.surface == nil,
              published.pointer != nil else {
            return nil
        }
        return (published.pointer!, published.byteCount)
    }

    /// OPTIMAL swapchain image as an MTLTexture via UTM scanout handle API.
    public func gpuMetalTexture(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> AnyObject? {
        let lookup: () -> AnyObject? = { [self] in
            guard !rendererTornDown,
                  let raw = np_venus_metal_texture(
                    venus, resourceID, UInt32(width), UInt32(height),
                    UInt32(bytesPerRow), format)
            else { return nil }
            return Unmanaged<AnyObject>.fromOpaque(raw).takeUnretainedValue()
        }
        if DispatchQueue.getSpecific(key: Self.deviceQueueKey) != nil { return lookup() }
        return deviceQueue.sync(execute: lookup)
    }

    private func buffer(forResource resourceID: UInt32) -> PublishedBuffer? {
        publishedLock.lock()
        defer { publishedLock.unlock() }
        return published[resourceID] ?? retiredPublished[resourceID]
    }

    private func publish(_ resource: GPUResource) {
        let entry = PublishedBuffer(
            surface: resource.surface,
            pointer: resource.baseAddress,
            byteCount: resource.byteCount)
        publishedLock.lock()
        retiredPublished.removeValue(forKey: resource.resourceID)
        retiredPublishedOrder.removeAll { $0 == resource.resourceID }
        published[resource.resourceID] = entry
        publishedLock.unlock()
    }

    private func unpublish(_ resourceID: UInt32) {
        publishedLock.lock()
        if let entry = published.removeValue(forKey: resourceID), entry.surface != nil {
            retiredPublished[resourceID] = entry
            retiredPublishedOrder.removeAll { $0 == resourceID }
            retiredPublishedOrder.append(resourceID)
            while retiredPublishedOrder.count > Self.retiredPublishedLimit {
                let oldest = retiredPublishedOrder.removeFirst()
                retiredPublished.removeValue(forKey: oldest)
            }
        }
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

    /// Address space only, so the default is generous rather than frugal.
    public static let defaultApertureSize: UInt64 = 4 << 30

    /// Size of the host-visible aperture this device advertises.
    public let apertureSize: UInt64

    public init(hostVisibleApertureSize: UInt64 = VirtioGPUDevice.defaultApertureSize) {
        apertureSize = hostVisibleApertureSize
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

        // numScanouts = 0 is how the guest is denied a display: Linux clears
        // DRIVER_MODESET and DRIVER_ATOMIC on seeing zero scanouts, leaving a
        // render node and no card0. The rule is enforced by the guest's own
        // driver rather than by anything we have to police.
        let deviceConfig = VirtioGPU.DeviceConfig(numScanouts: 0, numCapsets: 1)
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
            // The guest allocates resource ids and reuses them. DRM tears objects
            // down lazily, so RESOURCE_UNREF for the old resource can arrive after
            // the CREATE that reuses its id — and refusing the create as a
            // duplicate strands the surface permanently, which looks like a window
            // that goes blank after a resize and never comes back. The guest is
            // authoritative here: a create for a live id replaces it.
            if let stale = resources[request.resourceID] {
                Self.note("replace res=\(request.resourceID) (create arrived before unref)")
                unpublish(request.resourceID)
                unmapFromGuest(stale, element: nil, header: nil) { [weak self] in
                    guard let self, !self.rendererTornDown else {
                        element.returnToQueue()
                        return
                    }
                    np_venus_unimport_blob(self.venus, request.resourceID)
                    _ = self.resources.remove(request.resourceID)
                    self.createBlobResource(request, header: header, element: element)
                }
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
            guard let resource = resources[request.resourceID] else {
                respond(element, header.reply(.okNoData))
                return
            }
            unmapFromGuest(resource, element: nil, header: nil) { [weak self] in
                guard let self, !self.rendererTornDown else {
                    element.returnToQueue()
                    return
                }
                np_venus_unimport_blob(self.venus, request.resourceID)
                _ = self.resources.remove(request.resourceID)
                self.respond(element, header.reply(.okNoData))
            }

        case .submit3D:
            let request = try VirtioGPU.Submit3D(parsing: &reader)
            let payload = try reader.readBytes(Int(request.payloadByteCount))
            submitVenus(payload, header: header, element: element)

        case .getDisplayInfo, .setScanout, .setScanoutBlob, .resourceFlush, .getEDID:
            // There is no display. A guest asking for one has misunderstood the
            // device, and saying so is better than inventing a fake monitor.
            Self.log.error("rejecting display command \(String(describing: command), privacy: .public)")
            respond(element, header.reply(.errInvalidScanoutID))

        case .updateCursor, .moveCursor:
            // Cursor lives on the macOS side; nothing to draw here.
            respond(element, header.reply(.okNoData))

        case .resourceCreate2D, .transferToHost2D, .resourceAttachBacking,
             .resourceDetachBacking, .resourceCreate3D, .transferToHost3D,
             .transferFromHost3D, .resourceAssignUUID:
            Self.log.error("unimplemented command \(String(describing: command), privacy: .public)")
            respond(element, header.reply(.errUnspecified))
        }
    }

    // MARK: - Shared memory

    private func createBlobResource(
        _ request: VirtioGPU.ResourceCreateBlob,
        header: VirtioGPU.ControlHeader,
        element: VZVirtioQueueElement
    ) {
        let geometry = VirtioGPU.BlobGeometry(blobID: request.blobID)
        if geometry != nil {
            do {
                let resource = try resources.createBlob(
                    id: request.resourceID, size: request.size, geometry: geometry,
                    blobID: request.blobID)
                importIntoVenus(resource, contextID: header.contextID)
                finishCreateBlob(resource, geometry: geometry, header: header, element: element)
            } catch {
                Self.log.error("could not create compositor blob: \(error.localizedDescription, privacy: .public)")
                respond(element, header.reply(.errOutOfMemory))
            }
        } else if np_venus_is_live(venus) {
            adoptVenusBlob(request, header: header, element: element, attempt: 0)
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
        completion: (() -> Void)? = nil
    ) {
        guard let region = hostVisibleRegion, let offset = resource.mappedOffset else {
            if let element, let header { respond(element, header.reply(.okNoData)) }
            completion?()
            return
        }

        // Claim the mapping synchronously. Tearing down a resource sends
        // RESOURCE_UNMAP_BLOB and RESOURCE_UNREF back to back, and the unmap
        // completion runs later, so leaving `mappedOffset` set until then would
        // let the unref unmap the same range a second time.
        resource.mappedOffset = nil

        // Read back before the mapping goes away: whatever the guest wrote is
        // still sitting in these pages.
        Self.note("unmap   res=\(resource.resourceID) guest left \(Self.peek(resource))")

        enqueueRegionOperation { [weak self] done in
            guard let self, !rendererTornDown else {
                element?.returnToQueue()
                completion?()
                done()
                return
            }
            region.unmapMemory(atOffset: offset, size: UInt64(resource.byteCount)) { error in
                guard !self.rendererTornDown else {
                    element?.returnToQueue()
                    completion?()
                    done()
                    return
                }
                if let error {
                    Self.log.error("unmapping resource \(resource.resourceID) failed: \(error.localizedDescription, privacy: .public)")
                    Self.note("reject  unmap res=\(resource.resourceID) offset=\(offset): \(error.localizedDescription)")
                }
                if let element, let header {
                    self.respond(element, header.reply(.okNoData))
                }
                completion?()
                done()
            }
        }
    }

    // MARK: - Venus

    /// Compositor blob: tell vkr the resource exists. Failure is normal —
    /// packed geometry is not a Venus object id.
    private func importIntoVenus(_ resource: GPUResource, contextID: UInt32) {
        var blob = np_venus_blob(
            resource_id: resource.resourceID,
            blob_id: resource.blobID,
            blob_flags: VirtioGPU.BlobFlag.useMappable,
            pointer: resource.baseAddress,
            size: UInt64(resource.byteCount),
            width: UInt32(resource.geometry?.width ?? 0),
            height: UInt32(resource.geometry?.height ?? 0),
            bytes_per_row: UInt32(resource.geometry?.bytesPerRow ?? 0),
            iosurface: resource.surface.map { Unmanaged.passUnretained($0).toOpaque() })
        let rc = np_venus_create_blob(venus, contextID, &blob)
        if rc != 0 {
            Self.log.error("virglrenderer blob \(resource.resourceID) failed: \(rc)")
        }
    }

    /// Common tail of RESOURCE_CREATE_BLOB once the backing exists.
    private func finishCreateBlob(
        _ resource: GPUResource, geometry: VirtioGPU.BlobGeometry?,
        header: VirtioGPU.ControlHeader, element: VZVirtioQueueElement
    ) {
        // Probe words only on compositor IOSurfaces. A Venus mapping is
        // live GPU memory; stamping it would trash the allocation.
        if resource.surface != nil, let address = resource.baseAddress {
            let words = address.bindMemory(
                to: UInt32.self, capacity: min(4, resource.byteCount / 4))
            for i in 0..<min(4, resource.byteCount / 4) {
                words[i] = Self.hostProbePattern
            }
        }
        Self.log.info(
            "resource \(resource.resourceID) created, \(resource.byteCount) bytes")
        publish(resource)
        let shape = geometry.map { "\($0.width)x\($0.height)@\($0.bytesPerRow)" } ?? "linear"
        Self.note("create  res=\(resource.resourceID) size=\(resource.byteCount) \(shape)")
        notifyResourcePublished(resource.resourceID)
        respond(element, header.reply(.okNoData))
    }

    /// Mesa Venus blob: vkr holds (or is about to hold) the VkDeviceMemory.
    /// Bind it and adopt the host mapping so RESOURCE_MAP_BLOB shows the same
    /// pages. Retries are scheduled instead of blocking the device queue so
    /// that the ring doorbell queued behind this element can be processed.
    private func adoptVenusBlob(
        _ request: VirtioGPU.ResourceCreateBlob,
        header: VirtioGPU.ControlHeader,
        element: VZVirtioQueueElement,
        attempt: Int
    ) {
        var blob = np_venus_blob(
            resource_id: request.resourceID,
            blob_id: request.blobID,
            blob_flags: request.blobFlags,
            pointer: nil,
            size: request.size,
            width: 0, height: 0, bytes_per_row: 0,
            iosurface: nil)
        if np_venus_create_blob(venus, header.contextID, &blob) == 0 {
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
                finishCreateBlob(resource, geometry: nil, header: header, element: element)
            } catch {
                Self.log.error(
                    "could not adopt vkr mapping for res \(request.resourceID): \(error.localizedDescription, privacy: .public)")
                respond(element, header.reply(.errOutOfMemory))
            }
            return
        }
        guard attempt < 400 else {
            Self.note(
                "reject  create res=\(request.resourceID) blob_id=\(request.blobID) — vkr object never appeared")
            respond(element, header.reply(.errOutOfMemory))
            return
        }
        if attempt == 0 || attempt % 100 == 99 {
            Self.note(
                "wait    res=\(request.resourceID) blob_id=\(request.blobID) attempt=\(attempt)")
        }
        np_venus_poll(venus)
        deviceQueue.asyncAfter(deadline: .now() + .milliseconds(5)) { [weak self] in
            guard let self, !self.rendererTornDown else {
                element.returnToQueue()
                return
            }
            self.adoptVenusBlob(request, header: header, element: element, attempt: attempt + 1)
        }
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
        let resourceIDs = resources.identifiers
        let contextIDs = Array(contexts.keys)
        for resource in resources.mapped {
            unmapFromGuest(resource, element: nil, header: nil)
        }
        // This operation is queued after every asynchronous aperture unmap.
        // Renderer mappings must remain alive until the guest mapping is gone.
        enqueueRegionOperation { [self] done in
            for id in resourceIDs { np_venus_unimport_blob(venus, id) }
            for ctxID in contextIDs { np_venus_context_destroy(venus, ctxID) }
            contexts.removeAll()
            resources.removeAll()
            publishedLock.lock()
            published.removeAll(keepingCapacity: true)
            retiredPublished.removeAll(keepingCapacity: true)
            retiredPublishedOrder.removeAll(keepingCapacity: true)
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

        // `willStop` is the ownership boundary for the whole custom device.
        // Waiting for individual VZ shared-region unmaps here can deadlock:
        // Virtualization is allowed to stop delivering their asynchronous
        // completions once this callback returns. The guest can no longer
        // access the device, and VZ tears down the complete region itself, so
        // release renderer objects synchronously and let any late completion
        // take the guarded path above.
        let resourceIDs = resources.identifiers
        let contextIDs = Array(contexts.keys)
        for id in resourceIDs { np_venus_unimport_blob(venus, id) }
        for ctxID in contextIDs { np_venus_context_destroy(venus, ctxID) }
        contexts.removeAll()
        resources.removeAll()
        publishedLock.lock()
        published.removeAll(keepingCapacity: false)
        retiredPublished.removeAll(keepingCapacity: false)
        retiredPublishedOrder.removeAll(keepingCapacity: false)
        publishedLock.unlock()
        np_venus_destroy(venus)
        rendererDestroyed = true
    }
}
