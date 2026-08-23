import AppKit
import ImageIO
import IOSurface
@preconcurrency import Metal
import NativePipeProtocol
import UniformTypeIdentifiers

/// Where a committed frame's pixels come from.
///
/// Local scene layers name existing client or wl_shm-upload Venus textures. The
/// frame source exposes them as MTLTexture-shaped objects without making this
/// module depend on the virtual GPU implementation. Remote frames still arrive
/// as an IOSurface from VideoToolbox.
@MainActor
public protocol FrameSource: AnyObject {
    func surface(forResource resourceID: UInt32) -> IOSurfaceRef?
    /// True while this id names a live host resource. A published resource
    /// that still cannot produce the requested texture is not a publication
    /// race and must not hold a Wayland buffer forever.
    func isResourcePublished(_ resourceID: UInt32) -> Bool
    func metalTexture(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> AnyObject?
}

extension FrameSource {
    public func isResourcePublished(_ resourceID: UInt32) -> Bool { false }

    public func metalTexture(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> AnyObject? { nil }
}

/// Applies guest window events to `NSWindow`s, and sends host decisions back.
///
/// The guest owns Wayland state and resolves it to an immutable layer list.
/// This bridge resolves every resource id to its existing Metal texture and
/// asks the NSWindow to composite those textures into a drawable.
@MainActor
public final class WindowBridge {
    /// Window event tracing, off unless NATIVEPIPE_WINDOW_TRACE is set. Writes to
    /// stderr because in GUI mode the console window is the only other place
    /// diagnostics could go, and it cannot be read from a script.
    private static let frameTrace = ProcessInfo.processInfo.environment["NATIVEPIPE_FRAME_TRACE"] != nil
    private static let trace = frameTrace
        || ProcessInfo.processInfo.environment["NATIVEPIPE_WINDOW_TRACE"] != nil

    private static func note(_ message: @autoclosure () -> String) {
        guard trace else { return }
        FileHandle.standardError.write(Data("[win] \(message())\n".utf8))
    }

    struct CustomCursorGeometry: Equatable {
        let imageSize: CGSize
        let sourcePixels: CGRect
        let hotSpot: CGPoint
    }

    struct SurfaceLayerGeometry: Equatable {
        let bounds: CGRect
        let contentsRect: CGRect
        let contentsScale: CGFloat
    }

    /// Shared CALayer geometry for child surfaces. In particular, Firefox's
    /// rendering subsurface carries a 1600x1200 buffer, buffer_scale 1 and a
    /// viewport destination of 800x600; its layer must therefore be 800x600
    /// points while sampling all 1600x1200 pixels.
    static func surfaceLayerGeometry(
        frame: Windowing.Frame, allocationSize: CGSize
    ) -> SurfaceLayerGeometry {
        let logical = CGRect(origin: .zero, size: frame.appKitPointSize)
        return SurfaceLayerGeometry(
            bounds: logical,
            contentsRect: frame.contentsRect(
                for: logical, allocationSize: allocationSize),
            contentsScale: frame.pixelDensity(for: logical))
    }

    static func customCursorGeometry(
        frame: Windowing.Frame, hotSpot requestedHotSpot: CGPoint
    ) -> CustomCursorGeometry {
        let logical = frame.appKitPointSize
        let width = max(logical.width, 1)
        let height = max(logical.height, 1)
        return CustomCursorGeometry(
            imageSize: CGSize(width: width, height: height),
            sourcePixels: frame.fullViewportBufferPixelRect.integral,
            hotSpot: CGPoint(
                x: min(max(requestedHotSpot.x, 0), max(0, width - 1)),
                y: min(max(requestedHotSpot.y, 0), max(0, height - 1))))
    }

    /// A Wayland drag icon is neither a window nor part of the target surface.
    /// A non-activating, click-through panel gives it the same global, transient
    /// lifetime while AppKit continues to own window movement and hit testing.
    @MainActor
    private final class DragIconOverlay {
        private let panel: NSPanel
        private let view = NSView()
        private let metalLayer = CAMetalLayer()
        private var renderer: HostSceneRenderer?
        private var presenter: AsyncMetalScenePresenter?
        private weak var rendererDevice: MTLDevice?
        private var displayedTexture: MTLTexture?

        init() {
            panel = NSPanel(
                contentRect: .zero,
                styleMask: [.borderless, .nonactivatingPanel],
                backing: .buffered,
                defer: false)
            view.wantsLayer = true
            view.layer = metalLayer
            metalLayer.pixelFormat = .bgra8Unorm
            metalLayer.isOpaque = false
            metalLayer.framebufferOnly = true
            panel.contentView = view
            panel.backgroundColor = .clear
            panel.isOpaque = false
            panel.hasShadow = false
            panel.ignoresMouseEvents = true
            panel.level = .popUpMenu
            panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        }

        func display(
            _ texture: MTLTexture, frame: Windowing.Frame,
            readComplete: @escaping (Bool) -> Void,
            presented: @escaping () -> Void
        ) -> Bool {
            let scale = CGFloat(max(frame.scale, 1))
            let size = NSSize(
                width: CGFloat(frame.width) / scale,
                height: CGFloat(frame.height) / scale)
            panel.setContentSize(size)
            metalLayer.frame = view.bounds
            if renderer == nil || rendererDevice !== texture.device {
                renderer = try? HostSceneRenderer(device: texture.device)
                presenter = renderer.map {
                    AsyncMetalScenePresenter(
                        layer: metalLayer, device: texture.device, renderer: $0)
                }
                rendererDevice = texture.device
            }
            guard let presenter else { return false }
            displayedTexture = texture
            moveToPointer()
            panel.orderFrontRegardless()

            let source = frame.fullViewportBufferPixelRect
            let layer = Windowing.SceneLayer(
                surface: 1, resourceID: frame.resourceID,
                width: frame.width, height: frame.height,
                bytesPerRow: frame.bytesPerRow, format: frame.format,
                destination: .init(
                    x: 0, y: 0, width: Double(frame.width), height: Double(frame.height)),
                sourcePixels: .init(
                    x: Double(source.origin.x), y: Double(source.origin.y),
                    width: Double(source.width), height: Double(source.height)),
                clip: .init(
                    x: 0, y: 0, width: Double(frame.width), height: Double(frame.height)),
                alpha: 1, opaque: frame.format == .bgrx8888, transform: .normal)
            let scene = Windowing.SceneSnapshot(
                surface: 1, presentationID: frame.presentationID,
                width: frame.width, height: frame.height, scale: max(frame.scale, 1),
                windowGeometry: .init(
                    x: 0, y: 0, width: Int(size.width), height: Int(size.height)),
                layers: [layer])
            presenter.enqueue(
                scene: scene,
                layers: [ResolvedSceneLayer(state: layer, texture: texture)],
                readComplete: readComplete,
                latched: {},
                presented: presented)
            return true
        }

        func moveToPointer() {
            guard panel.isVisible else { return }
            let pointer = NSEvent.mouseLocation
            // Keep the image next to rather than underneath the pointer. The
            // panel ignores events either way, but the offset leaves the drop
            // target and cursor visually legible.
            panel.setFrameOrigin(NSPoint(
                x: pointer.x + 8,
                y: pointer.y - panel.frame.height - 8))
        }

        func hide() {
            panel.orderOut(nil)
            displayedTexture = nil
        }
    }

    private let dragIcon = DragIconOverlay()
    private var dragIconSurface: UInt32?
    /// A client normally commits the icon immediately before start_drag gives
    /// the surface its role, so retain unroled commits until that event arrives.
    private var pendingSurfaceFrames: [UInt32: Windowing.Frame] = [:]
    /// A commit can beat virtio CREATE_BLOB publication on the host. Resource
    /// publication is an event on the main actor, so no polling timer is needed.
    private var pendingFrames: [UInt32: Windowing.Frame] = [:]
    /// A binary scene can race CREATE_BLOB publication for any of its layers.
    /// Keep only the newest complete snapshot for each window until every
    /// resource is resolvable; superseded ids are completed explicitly.
    private var pendingScenes: [UInt32: Windowing.SceneSnapshot] = [:]
    private var pointerCursor = NSCursor.arrow

    private var windows: [UInt32: NativeWindow] = [:]
    private var mappedApplicationWindows: Set<UInt32> = []
    /// Surfaces that exist but have no role yet, and the toplevel each one backs.
    private var surfaceToWindow: [UInt32: UInt32] = [:]
    private var knownSurfaces: Set<UInt32> = []

    /// Strong on purpose. There is no cycle to break — a frame source refers to
    /// the VM controller weakly, if at all — and a weak reference here silently
    /// drops every frame the moment the caller stops holding the source itself.
    private let frameSource: FrameSource?

    /// Sends a command down to the guest translator. Wired to the vsock channel
    /// in the real path; the demo driver substitutes its own sink.
    public var output: ((Windowing.HostCommand) -> Void)?
    /// Fired once when a toplevel has both an app id and a materialized
    /// NSWindow. This is the launcher's end-to-end success signal.
    public var onApplicationWindowMapped: ((String) -> Void)?
    public var applicationIconProvider: ((String) -> NSImage?)?

    let clipboard = ClipboardBridge()

    public init(frameSource: FrameSource?) {
        self.frameSource = frameSource
        clipboard.output = { [weak self] command in self?.send(command) }
        clipboard.start()
    }

    public var windowCount: Int { windows.count }

    public func refreshApplicationIcons() {
        for window in windows.values { window.refreshApplicationIcon() }
    }

    func window(_ id: UInt32) -> NativeWindow? { windows[id] }

    func currentPointerCursor() -> NSCursor { pointerCursor }

    func applicationIcon(for applicationID: String) -> NSImage? {
        applicationIconProvider?(applicationID)
    }

    public func send(_ command: Windowing.HostCommand) {
        // Outgoing commands were the one direction with no trace, which made
        // "input does not work" impossible to localise from the logs alone.
        switch command {
        case .pointerMoved:
            dragIcon.moveToPointer()
            break  // every frame of mouse movement would drown everything else
        case .pointerScroll:
            break  // trackpads can report hundreds per second
        case .configure(_, _, let states, _) where states.contains(.resizing):
            if Self.frameTrace { Self.note("-> \(command)") }
        case .pointerEntered:
            dragIcon.moveToPointer()
            Self.note("-> \(command)")
        default:
            Self.note("-> \(command)")
        }
        output?(command)
    }

    // MARK: - Event application

    public func apply(_ event: Windowing.GuestEvent) {
        if case .sceneCommitted(let scene) = event {
            Self.note(
                "scene surface=\(scene.surface) present=\(scene.presentationID) " +
                "\(scene.width)x\(scene.height) layers=\(scene.layers.count)")
            for (index, layer) in scene.layers.enumerated() {
                Self.note(
                    "  layer[\(index)] surface=\(layer.surface) res=\(layer.resourceID) " +
                    "buffer=\(layer.width)x\(layer.height) destination=\(layer.destination) " +
                    "source=\(layer.sourcePixels) clip=\(layer.clip) " +
                    "format=\(layer.format) opaque=\(layer.opaque)")
            }
        } else if case .committed(let surface, let frame) = event {
            Self.note(
                "commit surface=\(surface) res=\(frame.resourceID) \(frame.width)x\(frame.height) source=\(frame.source)")
        } else {
            Self.note("\(event)")
        }

        switch event {
        case .channelReady:
            // Consumed by WindowChannel as the transport generation boundary.
            break

        case .surfaceCreated(let surface):
            knownSurfaces.insert(surface)

        case .surfaceDestroyed(let surface):
            knownSurfaces.remove(surface)
            if let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
                completeCopiedPresentation(
                    surface: surface, presentationID: frame.presentationID)
            }
            if let frame = pendingFrames.removeValue(forKey: surface) {
                completeCopiedPresentation(
                    surface: surface, presentationID: frame.presentationID)
            }
            if let scene = pendingScenes.removeValue(forKey: surface) {
                completeScene(scene)
            }
            if dragIconSurface == surface {
                dragIconSurface = nil
                dragIcon.hide()
            }
            if let windowID = surfaceToWindow.removeValue(forKey: surface) {
                mappedApplicationWindows.remove(windowID)
                windows.removeValue(forKey: windowID)?.close()
            }

        case .toplevelCreated(let window, let surface):
            // NSWindow waits for the first committed frame. Creating it here
            // injects scaleChanged + focus + another xdg configure while Mesa
            // is still in a Wayland roundtrip; Alpine vkcube SIGSEGVs there.
            let native = NativeWindow(windowID: window, surfaceID: surface, bridge: self)
            windows[window] = native
            surfaceToWindow[surface] = window
            if let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
                presentCommitted(surface: surface, windowID: window, frame: frame)
            }
            if let scene = pendingScenes.removeValue(forKey: surface) {
                present(scene: scene, windowID: window)
            }

        case .popupCreated(let window, let surface, let parent, let x, let y, _, _):
            // Like a toplevel, the NSWindow waits for the first frame; a menu
            // that flashes empty before it draws is worse than one that appears
            // a frame later.
            let native = NativeWindow(
                windowID: window, surfaceID: surface, bridge: self,
                popup: NativeWindow.Popup(parent: parent, origin: CGPoint(x: x, y: y)))
            windows[window] = native
            surfaceToWindow[surface] = window
            if let scene = pendingScenes.removeValue(forKey: surface) {
                present(scene: scene, windowID: window)
            }

        case .popupDestroyed(let window):
            if let native = windows.removeValue(forKey: window) {
                mappedApplicationWindows.remove(window)
                surfaceToWindow.removeValue(forKey: native.surfaceID)
                native.close()
            }

        case .toplevelDestroyed(let window):
            if let native = windows.removeValue(forKey: window) {
                mappedApplicationWindows.remove(window)
                surfaceToWindow.removeValue(forKey: native.surfaceID)
                native.close()
            }

        case .subsurfaceCreated, .subsurfaceMoved, .subsurfaceDestroyed:
            // Compatibility with an older guest. Current compositors consume
            // the surface tree and never publish per-child host state.
            break

        case .dragIconChanged(let surface):
            dragIconSurface = surface
            guard let surface else {
                dragIcon.hide()
                return
            }
            if let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
                guard let texture = texture(for: frame) else {
                    retainDeferred(frame, for: surface)
                    return
                }
                presentDragIcon(texture, frame: frame, surface: surface)
            }

        case .cursorChanged(let surface, _, _):
            // A custom surface is installed when its committed pixels arrive.
            // Until then retain the current AppKit cursor; nil restores arrow.
            if surface == nil { pointerCursor = .arrow }

        case .cursorShapeChanged(let shape):
            pointerCursor = NativeCursorResolver.cursor(for: shape)
            pointerCursor.set()

        case .titleChanged(let window, let title):
            windows[window]?.title = title

        case .appIDChanged(let window, let appID):
            windows[window]?.setAppID(appID)
            notifyApplicationWindowMapped(window)

        case .decorationModeChanged(let window, let serverSide):
            windows[window]?.setServerDecorated(serverSide)

        case .parentChanged(let window, let parent):
            windows[window]?.setParent(parent.flatMap { windows[$0] })

        case .sizeConstraintsChanged(let window, let minimum, let maximum):
            windows[window]?.setConstraints(minimum: minimum, maximum: maximum)

        case .committed(let surface, let frame):
            if surface == dragIconSurface {
                guard let texture = texture(for: frame) else {
                    retainDeferred(frame, for: surface)
                    Self.note("drag icon commit deferred: no Metal texture for \(frame.resourceID)")
                    return
                }
                pendingFrames.removeValue(forKey: surface)
                presentDragIcon(texture, frame: frame, surface: surface)
                return
            }
            guard let windowID = surfaceToWindow[surface] else {
                retainUnroled(frame, for: surface)
                Self.note("commit retained: surface \(surface) has no role yet")
                return
            }
            presentCommitted(surface: surface, windowID: windowID, frame: frame)

        case .sceneCommitted(let scene):
            guard let windowID = surfaceToWindow[scene.surface] else {
                retainDeferred(scene)
                Self.note("scene retained: surface \(scene.surface) has no role yet")
                return
            }
            present(scene: scene, windowID: windowID)

        case .frameCallbackRequested(let surface, let presentationID):
            schedulePresentation(surface: surface, presentationID: presentationID)

        case .interactiveMoveRequested(let window, _):
            // Client-side decorations report title-bar drags this way, which is
            // why the host never has to infer a draggable region.
            guard let nsWindow = windows[window]?.window,
                  let event = NSApp.currentEvent
            else { return }
            nsWindow.performDrag(with: event)

        case .interactiveResizeRequested:
            // Both server-decorated and borderless CSD windows carry AppKit's
            // .resizable style, so the real window edge owns the resize loop.
            // Manually changing frames here competes with AppKit hit testing.
            break

        case .fullscreenRequested(let window, let enabled):
            windows[window]?.setFullscreen(enabled)

        case .maximizeRequested(let window, let enabled):
            windows[window]?.setMaximized(enabled)

        case .textInputEnabled(let window, let enabled):
            windows[window]?.setTextInput(enabled: enabled)

        case .textInputCursorRect(let window, let x, let y, let width, let height):
            windows[window]?.setTextCursorRect(
                CGRect(x: x, y: y, width: max(width, 1), height: max(height, 1)))

        case .textInputSurroundingText:
            // Only useful for reconversion, which the host declines for now.
            break

        case .selectionOffered(let mimeTypes):
            clipboard.guestOffered(mimeTypes: mimeTypes)

        case .selectionData(let token, _, let base64):
            clipboard.guestSuppliedData(token: token, base64: base64)

        case .hostSelectionRequest(let token, let mimeType):
            clipboard.guestRequestedHostData(token: token, mimeType: mimeType)

        case .minimizeRequested(let window):
            windows[window]?.window?.miniaturize(nil)
        }
    }

    /// Called when a virtio-gpu resource becomes presentable after CREATE_BLOB.
    public func retryPendingFrames() {
        let scenes = pendingScenes
        for (surface, scene) in scenes {
            guard let windowID = surfaceToWindow[surface] else { continue }
            present(scene: scene, windowID: windowID)
        }
        guard !pendingFrames.isEmpty else { return }
        let snapshot = pendingFrames
        for (surface, frame) in snapshot {
            apply(.committed(surface: surface, frame: frame))
        }
    }

    private func retainDeferred(_ scene: Windowing.SceneSnapshot) {
        if let previous = pendingScenes.updateValue(scene, forKey: scene.surface),
           previous.presentationID != scene.presentationID {
            completeScene(previous)
        }
    }

    private func completeScene(_ scene: Windowing.SceneSnapshot) {
        send(.framePresented(
            surface: scene.surface, presentationID: scene.presentationID))
        send(.frameReleased(
            surface: scene.surface, presentationID: scene.presentationID))
    }

    private func present(scene: Windowing.SceneSnapshot, windowID: UInt32) {
        guard let native = windows[windowID], let frameSource else {
            retainDeferred(scene)
            return
        }
        var resolved: [ResolvedSceneLayer] = []
        resolved.reserveCapacity(scene.layers.count)
        for layer in scene.layers {
            let virglFormat: UInt32 = layer.format == .rgba8888 ? 67 : 1
            guard let object = frameSource.metalTexture(
                forResource: layer.resourceID,
                width: layer.width, height: layer.height,
                bytesPerRow: layer.bytesPerRow, format: virglFormat),
                let texture = object as? MTLTexture
            else {
                if frameSource.isResourcePublished(layer.resourceID) {
                    // The guest waits for every scene's frame/FIFO completion
                    // before it can submit the next swapchain image. Once the
                    // named resource exists, an export failure cannot be fixed
                    // by another CREATE_BLOB notification. Drop this frame and
                    // release its host-read references instead of deadlocking
                    // the whole client behind one unpresentable image.
                    pendingScenes.removeValue(forKey: scene.surface)
                    Self.note(
                        "scene discarded: published resource " +
                        "\(layer.resourceID) has no Metal texture")
                    completeScene(scene)
                    return
                }
                retainDeferred(scene)
                Self.note(
                    "scene deferred: no Metal texture for resource \(layer.resourceID)")
                return
            }
            resolved.append(ResolvedSceneLayer(state: layer, texture: texture))
        }
        pendingScenes.removeValue(forKey: scene.surface)
        native.present(
            scene: scene, layers: resolved,
            readComplete: { [weak self] success in
                guard let self else { return }
                self.send(.frameReleased(
                    surface: scene.surface,
                    presentationID: scene.presentationID))
            })
        notifyApplicationWindowMapped(windowID)
        native.traceLayerGeometry()
        injectTestInput(windowID)
        scheduleResizeProbe(native)
    }

    private func texture(for frame: Windowing.Frame) -> MTLTexture? {
        let format: UInt32 = frame.format == .rgba8888 ? 67 : 1
        return frameSource?.metalTexture(
            forResource: frame.resourceID,
            width: frame.width, height: frame.height,
            bytesPerRow: frame.bytesPerRow, format: format) as? MTLTexture
    }

    private func presentDragIcon(
        _ texture: MTLTexture, frame: Windowing.Frame, surface: UInt32
    ) {
        let queued = dragIcon.display(
            texture, frame: frame,
            readComplete: { [weak self] success in
                guard let self else { return }
                self.send(.frameReleased(
                    surface: surface, presentationID: frame.presentationID))
                if !success {
                    self.send(.framePresented(
                        surface: surface, presentationID: frame.presentationID))
                }
            },
            presented: { [weak self] in
                self?.send(.framePresented(
                    surface: surface, presentationID: frame.presentationID))
            })
        if !queued {
            completeCopiedPresentation(
                surface: surface, presentationID: frame.presentationID)
        }
    }

    /// Replace a frame the host has not installed only after completing the
    /// superseded presentation id. Once a commit crosses the guest/host
    /// boundary, that id retains its source texture until frameReleased.
    private func retainDeferred(_ frame: Windowing.Frame, for surface: UInt32) {
        if let previous = pendingFrames.updateValue(frame, forKey: surface),
           previous.presentationID != frame.presentationID {
            completeCopiedPresentation(
                surface: surface, presentationID: previous.presentationID)
        }
    }

    /// The same rule applies to a commit that arrives before its xdg role.
    private func retainUnroled(_ frame: Windowing.Frame, for surface: UInt32) {
        if let previous = pendingSurfaceFrames.updateValue(frame, forKey: surface),
           previous.presentationID != frame.presentationID {
            completeCopiedPresentation(
                surface: surface, presentationID: previous.presentationID)
        }
    }

    private func presentCommitted(surface: UInt32, windowID: UInt32, frame: Windowing.Frame) {
        guard let native = windows[windowID] else {
            Self.note("commit dropped: no window \(windowID)")
            return
        }
        guard frame.source == .encoded else {
            completeCopiedPresentation(
                surface: surface, presentationID: frame.presentationID)
            Self.note("ignored obsolete local committed frame for surface \(surface)")
            return
        }

        guard let ioSurface = frameSource?.surface(forResource: frame.resourceID) else {
            retainDeferred(frame, for: surface)
            Self.note("remote frame deferred: no decoded IOSurface for resource \(frame.resourceID)")
            return
        }
        pendingFrames.removeValue(forKey: surface)
        native.present(frame: frame, surface: ioSurface)
        notifyApplicationWindowMapped(windowID)
        dumpFrameIfRequested(frame, surfaceID: surface, surface: ioSurface)
        if Self.frameTrace {
            Self.note("installed window=\(windowID) res=\(frame.resourceID)")
        }
        native.traceLayerGeometry()
        injectTestInput(windowID)
        scheduleResizeProbe(native)
    }

    private func schedulePresentation(surface: UInt32, presentationID: UInt32) {
        guard presentationID != 0 else { return }
        if let native = nativeWindowOwningSurface(surface) ?? windows.values.first(where: { $0.window != nil }) {
            if nativeWindowOwningSurface(surface) != nil {
                native.awaitPresentation(surface: surface, presentationID: presentationID)
            } else {
                completeCopiedPresentation(
                    surface: surface, presentationID: presentationID)
            }
        } else {
            // An unroled or occluded surface has no latching deadline. FIFO v1
            // explicitly permits clearing its constraint early for forward
            // progress, and a frame callback must not deadlock the client.
            send(.framePresented(surface: surface, presentationID: presentationID))
            send(.frameReleased(surface: surface, presentationID: presentationID))
        }
    }

    private func completeCopiedPresentation(surface: UInt32, presentationID: UInt32) {
        guard presentationID != 0 else { return }
        send(.framePresented(surface: surface, presentationID: presentationID))
        send(.frameReleased(surface: surface, presentationID: presentationID))
    }

    private func nativeWindowOwningSurface(_ surface: UInt32) -> NativeWindow? {
        guard let windowID = surfaceToWindow[surface] else { return nil }
        return windows[windowID]
    }

    private func notifyApplicationWindowMapped(_ windowID: UInt32) {
        guard !mappedApplicationWindows.contains(windowID),
              let native = windows[windowID], !native.isPopup,
              native.window != nil,
              let appID = native.applicationID, !appID.isEmpty
        else { return }
        mappedApplicationWindows.insert(windowID)
        onApplicationWindowMapped?(appID)
    }

    /// Writes the first presented frame to NATIVEPIPE_WINDOW_DUMP, if set.
    ///
    /// Read straight out of the IOSurface the guest wrote through the aperture,
    /// so it is the actual memory CoreAnimation samples rather than a re-render.
    private static let dumpPath = ProcessInfo.processInfo.environment["NATIVEPIPE_WINDOW_DUMP"]
    private static let dumpDirectory = ProcessInfo.processInfo.environment["NATIVEPIPE_WINDOW_DUMP_DIR"]
    private var dumped = false
    private var dumpedFrameSignatures: Set<String> = []

    private func dumpFrameIfRequested(
        _ frame: Windowing.Frame, surfaceID: UInt32, surface: IOSurfaceRef
    ) {
        let path: String
        if let directory = Self.dumpDirectory {
            let signature = "\(surfaceID)-\(frame.resourceID)-\(frame.width)x\(frame.height)"
            guard dumpedFrameSignatures.count < 32,
                  dumpedFrameSignatures.insert(signature).inserted else { return }
            try? FileManager.default.createDirectory(
                atPath: directory, withIntermediateDirectories: true)
            path = (directory as NSString).appendingPathComponent("\(signature).png")
        } else {
            guard let firstPath = Self.dumpPath, !dumped else { return }
            dumped = true
            path = firstPath
        }

        IOSurfaceLock(surface, .readOnly, nil)
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }

        guard let context = CGContext(
            data: IOSurfaceGetBaseAddress(surface),
            width: frame.width,
            height: frame.height,
            bitsPerComponent: 8,
            bytesPerRow: frame.bytesPerRow,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue
                | CGBitmapInfo.byteOrder32Little.rawValue),
            let image = context.makeImage(),
            let destination = CGImageDestinationCreateWithURL(
                URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)
        else {
            Self.note("could not dump the first frame")
            return
        }
        CGImageDestinationAddImage(destination, image, nil)
        if CGImageDestinationFinalize(destination) {
            Self.note("wrote first frame to \(path)")
        }
    }

    /// Resizes a window on a timer when NATIVEPIPE_WINDOW_RESIZE_TEST is set, so
    /// the configure path can be exercised without a hand on the mouse.
    private static let resizeProbe = ProcessInfo.processInfo.environment["NATIVEPIPE_WINDOW_RESIZE_TEST"] != nil
    private var resizeProbeScheduled = false

    private func scheduleResizeProbe(_ native: NativeWindow) {
        guard Self.resizeProbe, !resizeProbeScheduled else { return }
        resizeProbeScheduled = true
        for (index, delay) in [12.0, 24.0, 36.0].enumerated() {
            let side = 300 + index * 140
            Timer.scheduledTimer(withTimeInterval: delay, repeats: false) { _ in
                MainActor.assumeIsolated {
                    guard let window = native.window else { return }
                    Self.note("resize probe -> \(side)x\(side * 2 / 3)")
                    window.setContentSize(NSSize(width: side, height: side * 2 / 3))
                }
            }
        }
    }

    /// Asks the client to take down every popup belonging to a window. Wayland
    /// models dismissal as a request, so the client is the one that destroys it.
    func dismissPopups(ownedBy parent: UInt32) {
        for (id, window) in windows where window.popup?.parent == parent {
            Self.note("dismissing popup \(id)")
            send(.dismissPopup(window: id))
        }
    }

    /// Injects a canned input sequence once the first frame lands, when
    /// NATIVEPIPE_INPUT_TEST is set.
    ///
    /// This separates two questions that otherwise have to be answered together:
    /// whether macOS delivers events to the content view, and whether the guest
    /// delivers them to the client. Only the second is testable without a hand
    /// on the keyboard.
    private static let injectInput = ProcessInfo.processInfo.environment["NATIVEPIPE_INPUT_TEST"] != nil
    private var injected = false

    private func injectTestInput(_ window: UInt32) {
        guard Self.injectInput, !injected else { return }
        injected = true
        Timer.scheduledTimer(withTimeInterval: 1.5, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                Self.note("injecting synthetic input")
                self.send(.keyboardFocus(window: window))
                self.send(.pointerEntered(window: window, x: 40, y: 40))
                self.send(.pointerMoved(window: window, x: 60, y: 50))
                self.send(.pointerButton(window: window, button: .left, pressed: true))
                self.send(.pointerButton(window: window, button: .left, pressed: false))
                // evdev 30 is A, 31 is S, 28 is Enter — enough for a shell
                // running `read` to complete a line and prove it got them.
                for code in [UInt32(30), UInt32(31), UInt32(28)] {
                    self.send(.key(window: window, keycode: code, pressed: true, modifiers: []))
                    self.send(.key(window: window, keycode: code, pressed: false, modifiers: []))
                }
                self.injectPopupGrabProbe(window)
            }
        }
    }

    /// Right-click, then type. A popup that took an xdg_popup.grab has to own
    /// the keyboard afterwards; if focus stayed on the toplevel the keys arrive
    /// at the surface the menu is covering, which is the bug this checks for.
    private func injectPopupGrabProbe(_ window: UInt32) {
        Timer.scheduledTimer(withTimeInterval: 1.5, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                Self.note("injecting right-click to open a grabbing popup")
                self.send(.pointerButton(window: window, button: .right, pressed: true))
                self.send(.pointerButton(window: window, button: .right, pressed: false))
            }
        }
        Timer.scheduledTimer(withTimeInterval: 3.0, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                Self.note("injecting keys that must land on the popup")
                // evdev 105/106 are Left/Right arrow — what menu navigation uses.
                for code in [UInt32(105), UInt32(106)] {
                    self.send(.key(window: window, keycode: code, pressed: true, modifiers: []))
                    self.send(.key(window: window, keycode: code, pressed: false, modifiers: []))
                }
            }
        }
    }

    public func closeAll() {
        for (surface, frame) in pendingSurfaceFrames {
            completeCopiedPresentation(
                surface: surface, presentationID: frame.presentationID)
        }
        pendingSurfaceFrames.removeAll()
        for (surface, frame) in pendingFrames {
            completeCopiedPresentation(
                surface: surface, presentationID: frame.presentationID)
        }
        pendingFrames.removeAll()
        for (_, scene) in pendingScenes { completeScene(scene) }
        pendingScenes.removeAll()
        for (_, window) in windows { window.close() }
        windows.removeAll()
        mappedApplicationWindows.removeAll()
        surfaceToWindow.removeAll()
        knownSurfaces.removeAll()
    }
}
