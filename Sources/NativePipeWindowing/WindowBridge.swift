import AppKit
import CoreGraphics
import CoreImage
import ImageIO
import IOSurface
@preconcurrency import Metal
import NativePipeProtocol
import UniformTypeIdentifiers

public enum FrameTextureStatus: Sendable {
    case ready
    case unpublished
    case unavailable
}

/// Result of resolving one exact scene layer. Only `unpublished` is retryable:
/// the CREATE_BLOB command has not crossed the virtio queue yet.
public struct FrameTextureResolution: @unchecked Sendable {
    public let status: FrameTextureStatus
    public let texture: AnyObject?

    public init(status: FrameTextureStatus, texture: AnyObject? = nil) {
        self.status = status
        self.texture = texture
    }
}

public struct DockWindow: Sendable, Identifiable, Equatable {
    public let id: UInt32
    public let title: String
    public let applicationID: String?
    public let isMiniaturized: Bool
    public let isZoomed: Bool
    public let isFullscreen: Bool

    public init(
        id: UInt32, title: String, applicationID: String?,
        isMiniaturized: Bool = false, isZoomed: Bool = false,
        isFullscreen: Bool = false
    ) {
        self.id = id
        self.title = title
        self.applicationID = applicationID
        self.isMiniaturized = isMiniaturized
        self.isZoomed = isZoomed
        self.isFullscreen = isFullscreen
    }
}

/// Where a committed frame's pixels come from.
///
/// Local scene layers name existing client or wl_shm-upload Venus textures. The
/// frame source exposes them as MTLTexture-shaped objects without making this
/// module depend on the virtual GPU implementation. Remote frames still arrive
/// as an IOSurface from VideoToolbox.
@MainActor
public protocol FrameSource: AnyObject {
    func surface(forResource resourceID: UInt32) -> IOSurfaceRef?
    /// True while this id names a live host resource. This distinguishes a
    /// CREATE_BLOB ordering race from a renderer import failure in diagnostics;
    /// neither case is allowed to fabricate an output latch.
    func isResourcePublished(_ resourceID: UInt32) -> Bool
    func metalTexture(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> AnyObject?
    func metalTextures(
        for layers: [Windowing.SceneLayer],
        completion: @escaping @MainActor ([FrameTextureResolution]) -> Void)
}

extension FrameSource {
    public func isResourcePublished(_ resourceID: UInt32) -> Bool { false }

    public func metalTexture(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> AnyObject? { nil }

    public func metalTextures(
        for layers: [Windowing.SceneLayer],
        completion: @escaping @MainActor ([FrameTextureResolution]) -> Void
    ) {
        completion(layers.map {
            let texture = metalTexture(
                forResource: $0.resourceID,
                width: $0.width, height: $0.height,
                bytesPerRow: $0.bytesPerRow,
                format: $0.format == .rgba8888 ? 67 : 1)
            if let texture {
                return FrameTextureResolution(status: .ready, texture: texture)
            }
            return FrameTextureResolution(
                status: isResourcePublished($0.resourceID)
                    ? .unavailable : .unpublished)
        })
    }
}

/// Applies guest window events to `NSWindow`s, and sends host decisions back.
///
/// The guest owns Wayland state and resolves it to an immutable layer list.
/// This bridge resolves every resource id to its existing Metal texture and
/// asks the NSWindow to composite those textures into a drawable.
@MainActor
public final class WindowBridge: NSObject {
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

    private static func report(_ message: String) {
        FileHandle.standardError.write(Data("[win] error: \(message)\n".utf8))
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

    static func topDownCursorImage(_ image: CIImage, source: CGRect) -> CIImage {
        image.cropped(to: source).transformed(by: CGAffineTransform(
            a: 1, b: 0, c: 0, d: -1,
            tx: 0, ty: source.minY + source.maxY))
    }

    /// A Wayland drag icon is neither a window nor part of the target surface.
    /// A non-activating, click-through panel gives it the same global, transient
    /// lifetime while AppKit continues to own window movement and hit testing.
    @MainActor
    private final class DragIconOverlay {
        private let panel: NSPanel
        private let view = NSView()
        private let metalLayer = CAMetalLayer()
        private var presenter: AsyncMetalScenePresenter?
		private var presenterRenderer: HostSceneRenderer?
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
            panel.isExcludedFromWindowsMenu = true
            panel.backgroundColor = .clear
            panel.isOpaque = false
            panel.hasShadow = false
            panel.ignoresMouseEvents = true
            panel.level = .popUpMenu
            panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        }

        func display(
            _ texture: MTLTexture, frame: Windowing.Frame,
			renderer: HostSceneRenderer,
            readComplete: @escaping (Bool) -> Void,
            presented: @escaping () -> Void
        ) -> Bool {
            let scale = CGFloat(max(frame.scale, 1))
            let size = NSSize(
                width: CGFloat(frame.width) / scale,
                height: CGFloat(frame.height) / scale)
            panel.setContentSize(size)
            metalLayer.frame = view.bounds
			if presenterRenderer !== renderer {
				presenter = AsyncMetalScenePresenter(
					layer: metalLayer, device: texture.device, renderer: renderer)
				presenterRenderer = renderer
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
                drawableSize: CGSize(width: frame.width, height: frame.height),
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
    /// Work that has not entered Metal yet. A superseded scene can release its
    /// source buffers immediately, but its frame callbacks/FIFO barrier must be
    /// inherited by the scene that really reaches the output latch.
    private struct SceneWork {
        var scene: Windowing.SceneSnapshot
        var latchIDs: [UInt32]

        init(scene: Windowing.SceneSnapshot) {
            self.scene = scene
            latchIDs = scene.presentationID == 0 ? [] : [scene.presentationID]
        }

        func superseding(_ older: SceneWork) -> SceneWork {
            var result = self
            result.scene = scene.includingUnrenderedDamage(from: older.scene)
            result.latchIDs = older.latchIDs + latchIDs.filter {
                !older.latchIDs.contains($0)
            }
            return result
        }
    }

    /// One latest-value waiting slot and at most one asynchronous resource
    /// lookup per surface. Resource lookup never blocks AppKit's main actor.
    private struct ResolvingScene {
        let token: UInt64
        let work: SceneWork
    }
    private var pendingScenes: [UInt32: SceneWork] = [:]
    private var resolvingScenes: [UInt32: ResolvingScene] = [:]
    private var nextSceneLookupToken: UInt64 = 0
    private var pointerCursor = NSCursor.arrow
    private var cursorSurface: UInt32?
    private var cursorHotSpot = CGPoint.zero
    private var cursorContext: CIContext?

    private var windows: [UInt32: NativeWindow] = [:]
    private var popupPlacements: [UInt32: Windowing.PopupPlacement] = [:]
    private var mappedApplicationWindows: Set<UInt32> = []
    /// Surfaces that exist but have no role yet, and the toplevel each one backs.
    private var surfaceToWindow: [UInt32: UInt32] = [:]
    private var knownSurfaces: Set<UInt32> = []
	private var sceneRenderers: [ObjectIdentifier: HostSceneRenderer] = [:]
	private let displayClock = DisplayClock()
	private var lastDisplays: [Windowing.Display] = []
	private struct WindowDisplayState: Equatable {
		let outputID: UInt32?
		let scale: Int
	}
	private var windowDisplayStates: [UInt32: WindowDisplayState] = [:]

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
		super.init()
        clipboard.output = { [weak self] command in self?.send(command) }
        clipboard.start()
		NotificationCenter.default.addObserver(
			self, selector: #selector(screenParametersChanged(_:)),
			name: NSApplication.didChangeScreenParametersNotification, object: nil)
    }

	deinit {
		NotificationCenter.default.removeObserver(self)
	}

    public var windowCount: Int { windows.count }

    /// Authoritative mapped xdg_toplevels for the VM host's window switcher.
    /// Popups, cursor surfaces and drag icons never become application windows.
    public var dockWindows: [DockWindow] {
        windows.values.compactMap { native in
            guard !native.isPopup, native.window != nil else { return nil }
            return DockWindow(
                id: native.windowID,
                title: native.title.isEmpty ? "Untitled Window" : native.title,
                applicationID: native.applicationID,
                isMiniaturized: native.isMiniaturized,
                isZoomed: native.isZoomed,
                isFullscreen: native.isFullscreen)
        }.sorted {
            $0.title.localizedStandardCompare($1.title) == .orderedAscending
        }
    }

    @discardableResult
    public func activateDockWindow(_ id: UInt32) -> Bool {
        guard let native = windows[id], !native.isPopup, native.window != nil else {
            return false
        }
        native.activateFromDock()
        return true
    }

    public func iconForDockWindow(_ id: UInt32) -> NSImage? {
        windows[id]?.dockIcon
    }

    @discardableResult
    public func minimizeDockWindow(_ id: UInt32) -> Bool {
        guard let native = dockWindow(id) else { return false }
        native.minimizeFromDock()
        return true
    }

    @discardableResult
    public func toggleZoomDockWindow(_ id: UInt32) -> Bool {
        guard let native = dockWindow(id) else { return false }
        native.toggleZoomFromDock()
        return true
    }

    @discardableResult
    public func toggleFullscreenDockWindow(_ id: UInt32) -> Bool {
        guard let native = dockWindow(id) else { return false }
        native.toggleFullscreenFromDock()
        return true
    }

    @discardableResult
    public func requestCloseDockWindow(_ id: UInt32) -> Bool {
        guard dockWindow(id) != nil else { return false }
        send(.close(window: id))
        return true
    }

    @discardableResult
    public func forceQuitDockWindow(_ id: UInt32) -> Bool {
        guard dockWindow(id) != nil else { return false }
        send(.forceQuit(window: id))
        return true
    }

    private func dockWindow(_ id: UInt32) -> NativeWindow? {
        guard let native = windows[id], !native.isPopup, native.window != nil else {
            return nil
        }
        return native
    }

    public func refreshApplicationIcons() {
        for window in windows.values { window.refreshApplicationIcon() }
    }

    func window(_ id: UInt32) -> NativeWindow? { windows[id] }

    func currentPointerCursor() -> NSCursor { pointerCursor }

    private func refreshPointerCursor() {
        for window in windows.values { window.refreshPointerCursor() }
    }

    func applicationIcon(for applicationID: String) -> NSImage? {
        applicationIconProvider?(applicationID)
    }

	func sceneRenderer(for device: MTLDevice) -> HostSceneRenderer? {
		let key = ObjectIdentifier(device as AnyObject)
		if let renderer = sceneRenderers[key] { return renderer }
		guard let renderer = try? HostSceneRenderer(device: device) else { return nil }
		sceneRenderers[key] = renderer
		return renderer
	}

	func registerDisplayClock(_ window: NativeWindow, screen: NSScreen?) {
		displayClock.register(window, screen: screen)
	}

	func unregisterDisplayClock(_ window: NativeWindow) {
		displayClock.unregister(window)
	}

	func windowScreenChanged(_ windowID: UInt32, screen: NSScreen?) {
		publishDisplayTopology()
		let state = WindowDisplayState(
			outputID: screen.flatMap { displayID(for: $0) },
			scale: screen.map { max(1, Int($0.backingScaleFactor.rounded())) } ?? 1)
		guard windowDisplayStates[windowID] != state else { return }
		windowDisplayStates[windowID] = state
		send(.windowOutputChanged(window: windowID, outputID: state.outputID))
		if state.outputID != nil {
			send(.scaleChanged(window: windowID, scale: state.scale))
		}
	}

	func windowClosed(_ windowID: UInt32) {
		guard windowDisplayStates.removeValue(forKey: windowID)?.outputID != nil else {
			return
		}
		send(.windowOutputChanged(window: windowID, outputID: nil))
	}

	@objc private func screenParametersChanged(_ notification: Notification) {
		publishDisplayTopology()
		for (windowID, native) in windows {
			registerDisplayClock(native, screen: native.window?.screen)
			windowScreenChanged(windowID, screen: native.window?.screen)
		}
	}

	private func displayID(for screen: NSScreen) -> UInt32? {
		(screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?
			.uint32Value
	}

	private func publishDisplayTopology(force: Bool = false) {
		let screens = NSScreen.screens
		let top = screens.map(\.frame.maxY).max() ?? 0
		let displays = screens.compactMap { screen -> Windowing.Display? in
			guard let id = displayID(for: screen) else { return nil }
			let frame = screen.frame
			let scale = max(1, Int(screen.backingScaleFactor.rounded()))
			let directID = CGDirectDisplayID(id)
			let physical = CGDisplayScreenSize(directID)
			let pixelWidth = CGDisplayPixelsWide(directID)
			let pixelHeight = CGDisplayPixelsHigh(directID)
			return Windowing.Display(
				id: id, name: screen.localizedName,
				x: Int(frame.minX.rounded()), y: Int((top - frame.maxY).rounded()),
				width: max(1, Int(frame.width.rounded())),
				height: max(1, Int(frame.height.rounded())),
				pixelWidth: max(1, pixelWidth), pixelHeight: max(1, pixelHeight),
				physicalWidthMM: max(0, Int(physical.width.rounded())),
				physicalHeightMM: max(0, Int(physical.height.rounded())),
				scale: scale,
				refreshMilliHz: max(1, screen.maximumFramesPerSecond) * 1_000)
		}.sorted { $0.id < $1.id }
		guard force || displays != lastDisplays else { return }
		lastDisplays = displays
		send(.outputsChanged(displays: displays))
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
			lastDisplays.removeAll(keepingCapacity: true)
			publishDisplayTopology(force: true)
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
            cancelSceneWork(for: surface)
            if dragIconSurface == surface {
                dragIconSurface = nil
                dragIcon.hide()
            }
            if cursorSurface == surface {
                cursorSurface = nil
                pointerCursor = .arrow
                refreshPointerCursor()
            }
            if let windowID = surfaceToWindow.removeValue(forKey: surface) {
                mappedApplicationWindows.remove(windowID)
                windows.removeValue(forKey: windowID)?.close()
            }

        case .surfaceUnmapped(let surface):
            if let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
                completeCopiedPresentation(
                    surface: surface, presentationID: frame.presentationID)
            }
            if let frame = pendingFrames.removeValue(forKey: surface) {
                completeCopiedPresentation(
                    surface: surface, presentationID: frame.presentationID)
            }
            cancelSceneWork(for: surface)
            guard let windowID = surfaceToWindow[surface],
                  let native = windows[windowID] else { break }
            mappedApplicationWindows.remove(windowID)
            // xdg-shell keeps the role alive across an unmap. Closing only the
            // AppKit object lets the next mapped scene recreate it with the
            // retained title, app id, decoration mode and constraints.
            native.close()

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
            startPendingScene(for: surface)

        case .popupCreated(let window, let surface, let parent, let x, let y, _, _):
            // Like a toplevel, the NSWindow waits for the first frame; a menu
            // that flashes empty before it draws is worse than one that appears
            // a frame later.
            let native = NativeWindow(
                windowID: window, surfaceID: surface, bridge: self,
                popup: NativeWindow.Popup(parent: parent, origin: CGPoint(x: x, y: y)))
            windows[window] = native
            surfaceToWindow[surface] = window
            startPendingScene(for: surface)

        case .popupPlacementRequested(let placement):
            popupPlacements[placement.window] = placement
            configurePopup(placement)

        case .popupRepositioned(let window, let x, let y, let width, let height):
            windows[window]?.applyPopupGeometry(
                origin: CGPoint(x: x, y: y),
                size: NSSize(width: width, height: height))
            parentGeometryChanged(window)

        case .popupDestroyed(let window):
            popupPlacements.removeValue(forKey: window)
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

        case .cursorChanged(let surface, let hotspotX, let hotspotY):
            cursorSurface = surface
            cursorHotSpot = CGPoint(x: hotspotX, y: hotspotY)
            guard let surface else {
                pointerCursor = .arrow
                refreshPointerCursor()
                break
            }
            if let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
                installCustomCursor(frame, surface: surface)
            }

        case .cursorShapeChanged(let shape):
            cursorSurface = nil
            pointerCursor = NativeCursorResolver.cursor(for: shape)
            refreshPointerCursor()

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
            if surface == cursorSurface {
                installCustomCursor(frame, surface: surface)
                return
            }
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
            enqueue(scene)
            guard surfaceToWindow[scene.surface] != nil else {
                Self.note("scene retained: surface \(scene.surface) has no role yet")
                return
            }
            startPendingScene(for: scene.surface)

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
        for surface in Array(pendingScenes.keys) { startPendingScene(for: surface) }
        if let surface = cursorSurface,
           let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
            installCustomCursor(frame, surface: surface)
        }
        if let surface = dragIconSurface,
           let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
            apply(.committed(surface: surface, frame: frame))
        }
        guard !pendingFrames.isEmpty else { return }
        let snapshot = pendingFrames
        for (surface, frame) in snapshot {
            apply(.committed(surface: surface, frame: frame))
        }
    }

    private func enqueue(_ scene: Windowing.SceneSnapshot) {
        var work = SceneWork(scene: scene)
        if let older = pendingScenes[scene.surface] {
            work = work.superseding(older)
            releaseScene(older.scene)
        }
        pendingScenes[scene.surface] = work
    }

    private func startPendingScene(for surface: UInt32) {
        guard resolvingScenes[surface] == nil,
              let work = pendingScenes[surface],
              let windowID = surfaceToWindow[surface],
              windows[windowID] != nil,
              let frameSource
        else { return }

        pendingScenes.removeValue(forKey: surface)
        nextSceneLookupToken &+= 1
        let token = nextSceneLookupToken
        resolvingScenes[surface] = ResolvingScene(token: token, work: work)
        frameSource.metalTextures(for: work.scene.layers) { [weak self] results in
            self?.resolvedScene(surface: surface, token: token, results: results)
        }
    }

    private func resolvedScene(
        surface: UInt32, token: UInt64,
        results: [FrameTextureResolution]
    ) {
        guard let resolving = resolvingScenes[surface], resolving.token == token else { return }
        resolvingScenes.removeValue(forKey: surface)
        let work = resolving.work

        // A scene committed while this lookup was in flight is authoritative.
        // The older source was never read by Metal, so release it and carry its
        // output-latch obligations into the newer work.
        if var newer = pendingScenes.removeValue(forKey: surface) {
            newer = newer.superseding(work)
            releaseScene(work.scene)
            pendingScenes[surface] = newer
            startPendingScene(for: surface)
            return
        }

        guard results.count == work.scene.layers.count else {
            discardScene(
                work,
                reason: "resolved \(results.count) textures for " +
                    "\(work.scene.layers.count) layers")
            return
        }

        var layers: [ResolvedSceneLayer] = []
        layers.reserveCapacity(work.scene.layers.count)
        var unpublished: [UInt32] = []
        var unavailable: [UInt32] = []
        for (state, result) in zip(work.scene.layers, results) {
            switch result.status {
            case .ready:
                guard let texture = result.texture as? MTLTexture else {
                    unavailable.append(state.resourceID)
                    continue
                }
                layers.append(ResolvedSceneLayer(state: state, texture: texture))
            case .unpublished:
                unpublished.append(state.resourceID)
            case .unavailable:
                unavailable.append(state.resourceID)
            }
        }
        if !unavailable.isEmpty {
            discardScene(
                work,
                reason: "resources \(unavailable) cannot export their committed Metal textures")
            return
        }
        guard unpublished.isEmpty else {
            pendingScenes[surface] = work
            Self.note("scene deferred: waiting for resources \(unpublished)")
            return
        }

        guard let windowID = surfaceToWindow[surface],
              let native = windows[windowID],
              native.present(
                scene: work.scene, layers: layers, latchIDs: work.latchIDs,
                readComplete: { [weak self] _ in
                    self?.releaseScene(work.scene)
                })
        else {
            discardScene(work, reason: "window presenter rejected the scene")
            return
        }

        notifyApplicationWindowMapped(windowID)
        native.traceLayerGeometry()
        injectTestInput(windowID)
        scheduleResizeProbe(native)
    }

    private func releaseScene(_ scene: Windowing.SceneSnapshot) {
        guard scene.presentationID != 0 else { return }
        send(.frameReleased(
            surface: scene.surface, presentationID: scene.presentationID))
    }

    private func complete(_ work: SceneWork) {
        releaseScene(work.scene)
        for presentationID in work.latchIDs where presentationID != 0 {
            send(.framePresented(
                surface: work.scene.surface, presentationID: presentationID))
        }
    }

    private func cancelSceneWork(for surface: UInt32) {
        if let work = pendingScenes.removeValue(forKey: surface) { complete(work) }
        if let resolving = resolvingScenes.removeValue(forKey: surface) {
            complete(resolving.work)
        }
    }

    /// Keep the last good image when one accepted guest resource cannot be
    /// exported. Its source is no longer read, while callbacks and FIFO retire
    /// on the next output latch instead of being fabricated immediately.
    private func discardScene(_ work: SceneWork, reason: String) {
        nativeWindowOwningSurface(work.scene.surface)?.invalidateSceneHistory()
        releaseScene(work.scene)
        for presentationID in work.latchIDs where presentationID != 0 {
            if let native = nativeWindowOwningSurface(work.scene.surface) {
                native.awaitPresentation(
                    surface: work.scene.surface, presentationID: presentationID)
            } else {
                send(.framePresented(
                    surface: work.scene.surface, presentationID: presentationID))
            }
        }
        Self.report(
            "discarded presentation \(work.scene.presentationID): \(reason)")
    }

    private func texture(for frame: Windowing.Frame) -> MTLTexture? {
        let format: UInt32 = frame.format == .rgba8888 ? 67 : 1
        return frameSource?.metalTexture(
            forResource: frame.resourceID,
            width: frame.width, height: frame.height,
            bytesPerRow: frame.bytesPerRow, format: format) as? MTLTexture
    }

    private func installCustomCursor(_ frame: Windowing.Frame, surface: UInt32) {
        guard let texture = texture(for: frame) else {
            retainUnroled(frame, for: surface)
            return
        }
        let geometry = Self.customCursorGeometry(
            frame: frame, hotSpot: cursorHotSpot)
        let source = geometry.sourcePixels.intersection(
            CGRect(x: 0, y: 0, width: texture.width, height: texture.height))
        guard !source.isEmpty,
              let image = CIImage(mtlTexture: texture, options: [
                .colorSpace: CGColorSpaceCreateDeviceRGB()
              ])
        else {
            completeCopiedPresentation(
                surface: surface, presentationID: frame.presentationID)
            return
        }
        if cursorContext == nil { cursorContext = CIContext(mtlDevice: texture.device) }
        // Core Image's Metal texture origin is bottom-left; Wayland viewport
        // coordinates are top-left.
        let ciSource = CGRect(
            x: source.minX,
            y: CGFloat(texture.height) - source.maxY,
            width: source.width,
            height: source.height)
        let cursorImage = Self.topDownCursorImage(image, source: ciSource)
        guard let cgImage = cursorContext?.createCGImage(cursorImage, from: ciSource) else {
            completeCopiedPresentation(
                surface: surface, presentationID: frame.presentationID)
            return
        }
        let nsImage = NSImage(cgImage: cgImage, size: geometry.imageSize)
        pointerCursor = NSCursor(image: nsImage, hotSpot: geometry.hotSpot)
        refreshPointerCursor()
        pendingSurfaceFrames.removeValue(forKey: surface)
        completeCopiedPresentation(
            surface: surface, presentationID: frame.presentationID)
    }

    private func presentDragIcon(
        _ texture: MTLTexture, frame: Windowing.Frame, surface: UInt32
    ) {
		guard let renderer = sceneRenderer(for: texture.device) else {
			completeCopiedPresentation(
				surface: surface, presentationID: frame.presentationID)
			return
		}
        let queued = dragIcon.display(
			texture, frame: frame, renderer: renderer,
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
        if let native = nativeWindowOwningSurface(surface) {
            native.awaitPresentation(surface: surface, presentationID: presentationID)
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

    func parentGeometryChanged(_ parent: UInt32) {
        for placement in popupPlacements.values
        where placement.parent == parent && placement.reactive {
            configurePopup(placement)
        }
    }

    private func configurePopup(_ placement: Windowing.PopupPlacement) {
        let bounds = windows[placement.parent]?.popupConstraintBounds
            ?? NSScreen.main.map {
                CGRect(origin: .zero, size: $0.visibleFrame.size)
            }
        guard let bounds else { return }
        let rect = Self.constrainPopup(placement, to: bounds)
        send(.configurePopup(
            window: placement.window,
            x: Int(rect.minX.rounded()), y: Int(rect.minY.rounded()),
            width: max(1, Int(rect.width.rounded())),
            height: max(1, Int(rect.height.rounded())),
            token: placement.token))
    }

    static func constrainPopup(
        _ placement: Windowing.PopupPlacement, to bounds: CGRect
    ) -> CGRect {
        func axis(
            origin: CGFloat, flipped: CGFloat, size: CGFloat,
            minimum: CGFloat, maximum: CGFloat,
            flip: Bool, slide: Bool, resize: Bool
        ) -> (CGFloat, CGFloat) {
            func fits(_ value: CGFloat, _ length: CGFloat) -> Bool {
                value >= minimum && value + length <= maximum
            }
            var value = origin
            var length = size
            if !fits(value, length), flip, fits(flipped, length) { value = flipped }
            if !fits(value, length), slide, length <= maximum - minimum {
                value = min(max(value, minimum), maximum - length)
            }
            if !fits(value, length), resize {
                let end = min(value + length, maximum)
                value = max(value, minimum)
                length = max(1, end - value)
            }
            return (value, length)
        }

        let bits = placement.adjustment
        let horizontal = axis(
            origin: CGFloat(placement.x), flipped: CGFloat(placement.flippedX),
            size: CGFloat(placement.width), minimum: bounds.minX, maximum: bounds.maxX,
            flip: bits & 4 != 0, slide: bits & 1 != 0, resize: bits & 16 != 0)
        let vertical = axis(
            origin: CGFloat(placement.y), flipped: CGFloat(placement.flippedY),
            size: CGFloat(placement.height), minimum: bounds.minY, maximum: bounds.maxY,
            flip: bits & 8 != 0, slide: bits & 2 != 0, resize: bits & 32 != 0)
        return CGRect(
            x: horizontal.0, y: vertical.0,
            width: horizontal.1, height: vertical.1)
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
        for work in pendingScenes.values { complete(work) }
        pendingScenes.removeAll()
        for resolving in resolvingScenes.values { complete(resolving.work) }
        resolvingScenes.removeAll()
        cursorSurface = nil
        cursorContext = nil
        pointerCursor = .arrow
        for (_, window) in windows { window.close() }
        windows.removeAll()
		windowDisplayStates.removeAll(keepingCapacity: true)
        popupPlacements.removeAll()
        mappedApplicationWindows.removeAll()
        surfaceToWindow.removeAll()
        knownSurfaces.removeAll()
    }
}
