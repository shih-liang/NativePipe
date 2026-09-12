import AppKit
@preconcurrency import IOSurface
@preconcurrency import Metal
import NativePipeProtocol
import QuartzCore

/// One refresh source per physical NSScreen, shared by every NativeWindow on
/// that display. A window moving screens is re-registered atomically; links are
/// invalidated when their final window leaves.
@MainActor
final class DisplayClock: NSObject {
	private final class WeakWindow {
		weak var value: NativeWindow?
		init(_ value: NativeWindow) { self.value = value }
	}
	private struct Entry {
		let link: CADisplayLink
		var windows: [ObjectIdentifier: WeakWindow]
	}
	private var entries: [ObjectIdentifier: Entry] = [:]
	private var screenByWindow: [ObjectIdentifier: ObjectIdentifier] = [:]

	func register(_ window: NativeWindow, screen: NSScreen?) {
		guard let screen = screen ?? NSScreen.main else { return }
		let windowKey = ObjectIdentifier(window)
		let screenKey = ObjectIdentifier(screen)
		if screenByWindow[windowKey] == screenKey { return }
		unregister(window)
		if entries[screenKey] == nil {
			let link = screen.displayLink(
				target: self, selector: #selector(tick(_:)))
			link.add(to: .main, forMode: .common)
			link.isPaused = false
			entries[screenKey] = Entry(link: link, windows: [:])
		}
		entries[screenKey]?.windows[windowKey] = WeakWindow(window)
		screenByWindow[windowKey] = screenKey
	}

	func unregister(_ window: NativeWindow) {
		let windowKey = ObjectIdentifier(window)
		guard let screenKey = screenByWindow.removeValue(forKey: windowKey),
			var entry = entries[screenKey] else { return }
		entry.windows.removeValue(forKey: windowKey)
		if entry.windows.isEmpty {
			entry.link.invalidate()
			entries.removeValue(forKey: screenKey)
		} else {
			entries[screenKey] = entry
		}
	}

	@objc private func tick(_ link: CADisplayLink) {
		guard let screenKey = entries.first(where: { $0.value.link === link })?.key,
			var entry = entries[screenKey] else { return }
		let windows = entry.windows
		for (key, weakWindow) in windows {
			if let window = weakWindow.value {
				window.displayClockFired(link)
			} else {
				entry.windows.removeValue(forKey: key)
				screenByWindow.removeValue(forKey: key)
			}
		}
		if entry.windows.isEmpty {
			entry.link.invalidate()
			entries.removeValue(forKey: screenKey)
		} else {
			entries[screenKey] = entry
		}
	}
}

@MainActor
private final class FrameCaptureWaiter {
    private var continuation: CheckedContinuation<RenderedFrameCapture, Error>?

    init(_ continuation: CheckedContinuation<RenderedFrameCapture, Error>) {
        self.continuation = continuation
    }

    func finish(_ result: Result<RenderedFrameCapture, Error>) {
        guard let continuation else { return }
        self.continuation = nil
        continuation.resume(with: result)
    }
}

@MainActor
private final class ScenePresentationCompletion {
    private var callback: ((Bool) -> Void)?
    init(_ callback: @escaping (Bool) -> Void) { self.callback = callback }
    func finish(_ displayed: Bool) {
        let callback = self.callback
        self.callback = nil
        callback?(displayed)
    }
}

/// One `xdg_toplevel`, one `NSWindow`.
///
/// This class is a translator and nothing else. There is no scene graph, no
/// stacking policy, no shadow, no decoration drawing and no compositing pass —
/// macOS already does all of that, and duplicating any of it would mean doing
/// the work twice and then fighting about which answer wins.
///
/// The guest compositor resolves one atomic scene description per window. This
/// window composites the scene's existing Venus/Metal textures directly into a
/// `CAMetalLayer` drawable. Source-buffer ownership ends when Metal completes;
/// CoreAnimation owns only its drawable pool and never owns a guest buffer.
///
///   * client buffers + guest-resolved layer state
///   * host Metal blit/render → CAMetalDrawable → WindowServer
@MainActor
final class NativeWindow: NSObject {
    private static let frameTrace = ProcessInfo.processInfo.environment["NATIVEPIPE_FRAME_TRACE"] != nil
    private static let trace = frameTrace
        || ProcessInfo.processInfo.environment["NATIVEPIPE_WINDOW_TRACE"] != nil
    private static let inputTrace = ProcessInfo.processInfo.environment["NATIVEPIPE_INPUT_TRACE"] != nil

    private static func note(_ message: @autoclosure () -> String) {
        guard trace else { return }
        FileHandle.standardError.write(Data("[nsw] \(message())\n".utf8))
    }

    let windowID: UInt32
    let surfaceID: UInt32
    private(set) var displayIntervalNanoseconds: UInt32 = 0

    /// A popup is a menu, dropdown or tooltip: borderless, anchored to a parent,
    /// and dismissed rather than closed. `origin` is where the client asked for
    /// it, in the parent's surface-local points.
    struct Popup {
        var parent: UInt32
        var origin: CGPoint
    }
    private(set) var popup: Popup?

    private(set) var window: NSWindow?
    private let contentView = SurfaceView()
    private weak var bridge: WindowBridge?
    private weak var transientParent: NativeWindow?

    private var appID: String?
    private var applicationIcon: NSImage?
    private var scrollGestureActive = false
    private var leftButtonPressed = false
    private var moveMouseDown: NSEvent?
    private var nativeMoveInProgress = false
    private var moveHandoffTime: Double?
    /// This is a value supplied by the guest compositor, not a host policy.
    /// Client-side is the safe construction default: it prevents an NSWindow
    /// titlebar from flashing around the first CSD frame before the protocol
    /// event arrives. Qt and other SSD clients explicitly request server-side.
    private var serverDecorated = false
    private var minimumConstraint: Windowing.Size?
    private var maximumConstraint: Windowing.Size?
    private var requestedMaximized: Bool?
    private var requestedFullscreen: Bool?
    private var lastConfiguredSize: Windowing.Size?
    private var lastConfiguredStates: [Windowing.ToplevelState] = []
    private var configureSerial: UInt32 = 0
	private var liveResizeStartFrame: NSRect?
	private var pendingResizeCompletionAnchor: WindowFrameAnchor?
	private var awaitingResizeCommitSerial: UInt32?
	private var resizeCommitAnchor: WindowFrameAnchor?
	private var applyingCommittedGeometry = false

    /// The xdg-shell window within the full wl_surface. GTK CSD buffers include
    /// transparent shadow margins outside this rectangle. AppKit must size and
    /// clip to the geometry while Wayland input remains surface-local.
    private var windowGeometry = Windowing.Rect(x: 0, y: 0, width: 1, height: 1)

    /// Resize events may arrive more often than either a display refresh or a
    /// Wayland client can paint. Keep the latest state for this main-run-loop
    /// turn; unlike the old in-flight gate, this never waits for a client frame
    /// and therefore cannot freeze a live resize.
    private struct PendingConfigure {
        var size: Windowing.Size
        var states: [Windowing.ToplevelState]
		/// State transitions at an interactive resize boundary must produce a
		/// configure even when their size matches the last emitted record.
		var force: Bool
    }
    private var pendingConfigure: PendingConfigure?
	private var presenterNeedsDisplayRetry = false
	/// Commits accepted since the previous display tick. This is the Wayland
	/// output-latch clock; Metal source release remains tied to command completion.
    private struct Presentation: Hashable {
        let surface: UInt32
        let id: UInt32
    }
    private var pendingPresentations: [Presentation] = []
    private let metalDevice = MTLCreateSystemDefaultDevice()
    private lazy var sceneRenderer: HostSceneRenderer? = {
        guard let metalDevice else { return nil }
		return bridge?.sceneRenderer(for: metalDevice)
    }()
    private lazy var asyncScenePresenter: AsyncMetalScenePresenter? = {
        guard let metalDevice, let renderer = sceneRenderer else { return nil }
        let markRetry: @MainActor @Sendable () -> Void = { [weak self] in
            self?.presenterNeedsDisplayRetry = true
        }
        return AsyncMetalScenePresenter(
            layer: contentView.metalLayer, device: metalDevice, renderer: renderer,
            requestDisplayRetry: {
                MainRunLoop.perform { markRetry() }
            })
    }()

    init(windowID: UInt32, surfaceID: UInt32, bridge: WindowBridge, popup: Popup? = nil) {
        self.windowID = windowID
        self.surfaceID = surfaceID
        self.bridge = bridge
        self.popup = popup
        super.init()
    }

    var isPopup: Bool { popup != nil }
    var applicationID: String? { appID ?? popup.flatMap { bridge?.window($0.parent)?.applicationID } }
    var dockIcon: NSImage? { applicationIcon }
    var isMiniaturized: Bool { window?.isMiniaturized == true }
    var isZoomed: Bool { window?.isZoomed == true }
    var isFullscreen: Bool { window?.styleMask.contains(.fullScreen) == true }
    var pointerCursor: NSCursor { bridge?.currentPointerCursor() ?? .arrow }

    func refreshPointerCursor() {
        window?.invalidateCursorRects(for: contentView)
    }

    func activateFromDock() {
        guard let window, !isPopup else { return }
        if window.isMiniaturized { window.deminiaturize(nil) }
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(contentView)
    }

    func minimizeFromDock() {
        guard let window, !isPopup else { return }
        window.performMiniaturize(nil)
    }

    func toggleZoomFromDock() {
        guard let window, !isPopup else { return }
        if window.isMiniaturized { window.deminiaturize(nil) }
        window.zoom(nil)
    }

    func toggleFullscreenFromDock() {
        guard let window, !isPopup else { return }
        if window.isMiniaturized { window.deminiaturize(nil) }
        window.toggleFullScreen(nil)
    }

    /// The guest compositor sends one already-composited scene per window.
    /// This is the only content layer at the host boundary.
    var rootSurfaceLayer: CALayer? { window != nil ? contentView.surfaceLayer : nil }

    // MARK: - Metadata

    var title: String = "" {
        didSet { window?.title = title }
    }

    func setAppID(_ value: String) {
        // Deliberately not setFrameAutosaveName: restoring a saved frame would
        // resize the window behind the client's back, and the resulting
        // configure would make the client redraw at a size nobody asked for.
        // Window geometry belongs to the client's first frame and then to the
        // user.
        appID = value
        refreshApplicationIcon()
    }

    func refreshApplicationIcon() {
        applicationIcon = appID.flatMap { bridge?.applicationIcon(for: $0) }
        window?.miniwindowImage = applicationIcon
    }

    func setServerDecorated(_ enabled: Bool) {
        guard serverDecorated != enabled else { return }
        serverDecorated = enabled
        guard let window, !isPopup else { return }
        let contentRect = window.contentRect(forFrameRect: window.frame)
        window.styleMask = styleMaskForToplevel()
        applyToplevelAppearance(to: window)
        window.setFrame(window.frameRect(forContentRect: contentRect), display: true)
        // CSD hides the title visually, but NSWindow.title remains the semantic
        // xdg_toplevel title used by Dock menus, Window menu and accessibility.
        window.title = title
    }

    func setConstraints(minimum: Windowing.Size?, maximum: Windowing.Size?) {
        minimumConstraint = minimum
        maximumConstraint = maximum
        applyConstraints()
    }

    func setMaximized(_ enabled: Bool) {
        requestedMaximized = enabled
        guard let window, window.isZoomed != enabled else { return }
        window.zoom(nil)
    }

    func setFullscreen(_ enabled: Bool) {
        requestedFullscreen = enabled
        guard let window,
              window.styleMask.contains(.fullScreen) != enabled
        else { return }
        window.toggleFullScreen(nil)
    }

    private func applyConstraints() {
        guard let window else { return }
        window.contentMinSize = minimumConstraint.map {
            NSSize(width: $0.width, height: $0.height)
        } ?? .zero
        window.contentMaxSize = maximumConstraint.map {
            NSSize(
                width: $0.width == 0 ? 10_000_000 : $0.width,
                height: $0.height == 0 ? 10_000_000 : $0.height)
        } ?? NSSize(width: 10_000_000, height: 10_000_000)
    }

    func setParent(_ parent: NativeWindow?) {
        transientParent = parent
        applyParent()
    }

    private func applyParent() {
        guard let window else { return }
        if let existing = window.parent {
            existing.removeChildWindow(window)
        }
        transientParent?.window?.addChildWindow(window, ordered: .above)
    }

    // MARK: - Content

    /// xdg_toplevel is already a window. Show it at the compositor's
    /// configure size so a late or missing first GPU frame is not invisible.
    func revealToplevel(width: Int = 800, height: Int = 600) {
        guard window == nil, !isPopup else { return }
        prepareWindow(for: Windowing.Frame(
            resourceID: 0, width: width, height: height,
            bytesPerRow: width * 4, format: .bgra8888, scale: 1))
    }

    func present(frame: Windowing.Frame, surface: IOSurfaceRef, owner: AnyObject? = nil) {
        prepareWindow(for: frame)
        let incoming = frame.presentationID == 0 ? nil : Presentation(
            surface: surfaceID, id: frame.presentationID)
        contentView.displayCPU(
            surface, frame: frame, geometry: windowGeometry, owner: owner)
        if let incoming {
            bridge?.send(.frameReleased(
                surface: incoming.surface, presentationID: incoming.id))
            awaitPresentation(incoming)
        }
        if Self.frameTrace {
            Self.note(
                "present window=\(windowID) res=\(frame.resourceID) " +
                "active=\(frame.width)x\(frame.height) stride=\(frame.bytesPerRow) " +
                "allocation=\(IOSurfaceGetWidth(surface))x\(IOSurfaceGetHeight(surface)) " +
                "allocationStride=\(IOSurfaceGetBytesPerRow(surface)) " +
                "scale=\(frame.scale) geometry=\(windowGeometry) " +
                "viewBounds=\(contentView.bounds)")
        }
    }


    /// Draw a complete guest-resolved scene directly from the client's existing
    /// Metal textures. `readComplete` is distinct from display presentation:
    /// the former releases Wayland buffers, the latter completes frame/FIFO.
    @discardableResult
    func present(
        scene: Windowing.SceneSnapshot, layers: [ResolvedSceneLayer],
        latchIDs: [UInt32],
        readComplete: @escaping @MainActor @Sendable (Bool) -> Void
    ) -> Bool {
        prepareWindow(for: scene)
        guard let presenter = asyncScenePresenter,
              let drawableSize = contentView.configureMetalLayer(scene: scene)
        else {
            Self.note("could not configure Metal scene window=\(windowID)")
            return false
        }
        let completion: ScenePresentationCompletion?
        let generation = bridge?.connectionGeneration
        if bridge?.onScenePresentation != nil {
            // A drawable can finish after its NSWindow has been destroyed.
            // Return its remote credit for as long as the session is alive.
            completion = ScenePresentationCompletion { [weak bridge = bridge] displayed in
                guard bridge?.connectionGeneration == generation else { return }
                bridge?.scenePresented(surface: scene.surface,
                    presentationID: scene.presentationID, displayed: displayed)
            }
        } else { completion = nil }
        let onPresented: (@MainActor @Sendable (Bool) -> Void)?
        if let completion { onPresented = { displayed in completion.finish(displayed) } }
        else { onPresented = nil }
        presenter.enqueue(
            scene: scene, layers: layers, drawableSize: drawableSize,
            readComplete: { success in
                readComplete(success)
                if !success { completion?.finish(false) }
            },
            latched: { [weak self] in
                guard self?.bridge?.connectionGeneration == generation else { return }
                for presentationID in latchIDs {
                    self?.awaitPresentation(
                        surface: scene.surface, presentationID: presentationID)
                }
            },
            presented: onPresented)
        return true
    }

    func awaitPresentation(surface: UInt32, presentationID: UInt32) {
        guard presentationID != 0 else { return }
        awaitPresentation(Presentation(surface: surface, id: presentationID))
    }

	func invalidateSceneHistory() {
		asyncScenePresenter?.invalidateDrawableAges()
	}

    /// Capture the next compositor-protected presentation. The guest republishes
    /// current state even when the client is idle, so static windows work without
    /// retaining a CAMetalDrawable or adding a per-frame history copy.
    func captureFrame() async throws -> RenderedFrameCapture {
        guard let presenter = asyncScenePresenter, bridge != nil else {
            throw ComputerUseWindowError.captureFailed
        }
        return try await withCheckedThrowingContinuation { continuation in
            let waiter = FrameCaptureWaiter(continuation)
            let requestID = presenter.captureNextFrame { result in waiter.finish(result) }
            // An explicit screenshot may target an occluded window. Allow its
            // capture scene through without continuously enabling rendering.
            bridge?.flushSceneFeedback(surface: surfaceID)
            bridge?.send(.captureFrame(surface: surfaceID))
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                presenter.cancelCapture(requestID)
                waiter.finish(.failure(ComputerUseWindowError.captureFailed))
            }
        }
    }

    private func awaitPresentation(_ presentation: Presentation) {
        guard !pendingPresentations.contains(presentation) else { return }
		guard let window, window.isVisible,
		      window.occlusionState.contains(.visible) else {
			bridge?.send(.framePresented(
				surface: presentation.surface, presentationID: presentation.id))
			return
		}
        pendingPresentations.append(presentation)
    }

    func traceLayerGeometry() {
        guard Self.frameTrace else { return }
        contentView.traceLayerGeometry(windowID: windowID)
    }

    private func prepareWindow(for frame: Windowing.Frame) {
        windowGeometry = effectiveGeometry(for: frame)
        let pointSize = NSSize(
            width: CGFloat(windowGeometry.width),
            height: CGFloat(windowGeometry.height))
        if window == nil {
            makeWindow(contentSize: pointSize)
        }
    }

    private func prepareWindow(for scene: Windowing.SceneSnapshot) {
        windowGeometry = scene.windowGeometry
        let pointSize = NSSize(
            width: CGFloat(scene.windowGeometry.width),
            height: CGFloat(scene.windowGeometry.height))
        if window == nil {
            makeWindow(contentSize: pointSize)
        } else if let window, let popup, window.contentView?.bounds.size != pointSize {
            // xdg_surface.window_geometry is double-buffered state. Firefox can
            // commit its final menu geometry after the first popup frame; keep
            // the native panel on that committed visible bound. Toplevel size
            // remains AppKit/configure-owned and must not follow stale frames.
            window.setContentSize(pointSize)
            position(window, forPopup: popup, size: pointSize)
		} else if !isPopup {
			applyCommittedToplevelGeometry(
				pointSize, configureSerial: scene.configureSerial)
        }
    }

	/// `xdg_toplevel.configure` supplies a size hint; the corresponding committed
	/// window geometry is the actual result. During live resize AppKit remains the
	/// provisional frame owner. After mouse-up, ignore older scenes until the
	/// final configure generation is committed, then accept the client's result
	/// without feeding the programmatic frame change back as another configure.
	private func applyCommittedToplevelGeometry(
		_ contentSize: NSSize, configureSerial committedSerial: UInt32
	) {
		guard let window, !window.inLiveResize,
		      contentSize.width > 0, contentSize.height > 0 else { return }
		if let required = awaitingResizeCommitSerial {
			guard Self.serial(committedSerial, isAtOrAfter: required) else { return }
			awaitingResizeCommitSerial = nil
		}

		let currentSize = contentView.bounds.size
		guard abs(currentSize.width - contentSize.width) > 0.5 ||
		      abs(currentSize.height - contentSize.height) > 0.5 else {
			resizeCommitAnchor = nil
			return
		}

		let targetFrameSize: NSSize
		if serverDecorated {
			targetFrameSize = window.frameRect(
				forContentRect: NSRect(origin: .zero, size: contentSize)).size
		} else {
			// A full-size CSD content view covers the complete AppKit frame.
			targetFrameSize = contentSize
		}
		let anchor = resizeCommitAnchor ?? .topLeft
		let target = anchor.frame(size: targetFrameSize, relativeTo: window.frame)
		resizeCommitAnchor = nil
		applyingCommittedGeometry = true
		window.setFrame(target, display: true)
		applyingCommittedGeometry = false
	}

	private static func serial(_ candidate: UInt32, isAtOrAfter reference: UInt32) -> Bool {
		guard candidate != 0 else { return false }
		return Int32(bitPattern: candidate &- reference) >= 0
	}

    private func effectiveGeometry(for frame: Windowing.Frame) -> Windowing.Rect {
        if let geometry = frame.windowGeometry,
           geometry.width > 0, geometry.height > 0 {
            return geometry
        }
        let logical = frame.appKitPointSize
        return Windowing.Rect(
            x: 0, y: 0,
            width: max(1, Int(logical.width.rounded())),
            height: max(1, Int(logical.height.rounded())))
    }

    private func makeWindow(contentSize: NSSize) {
        let window: NSWindow
        if isPopup {
            // xdg_popup is an independent transient surface, not a slice of the
            // parent's NSWindow.  Making it an AppKit child window lets the
            // parent's full-size transparent title bar participate in its
            // ordering/composition, visibly splitting a menu that straddles the
            // CSD title-bar boundary.  A non-activating menu-level panel is the
            // native equivalent: independently composited, above its owner, but
            // without taking keyboard focus from it.
            let panel = NSPanel(
                contentRect: NSRect(origin: .zero, size: contentSize),
                styleMask: [.borderless, .nonactivatingPanel],
                backing: .buffered,
                defer: false)
            panel.level = .popUpMenu
            panel.isFloatingPanel = true
            panel.becomesKeyOnlyIfNeeded = true
            panel.collectionBehavior = [.transient, .fullScreenAuxiliary]
            panel.isExcludedFromWindowsMenu = true
            // GTK supplies alpha for rounded corners and the menu outline. If
            // AppKit composites that over NSWindow's default background, the
            // transparent top edge becomes a conspicuous white strip.
            panel.isOpaque = false
            panel.backgroundColor = .clear
            window = panel
        } else {
            window = NSWindow(
                contentRect: NSRect(origin: .zero, size: contentSize),
                styleMask: styleMaskForToplevel(),
                backing: .buffered,
                defer: false)
        }
        // Swift ARC owns the NSWindow through `self.window`. AppKit's legacy
        // release-on-close ownership would otherwise deallocate it inside
        // `close()` while ARC still holds the same object, which is observable
        // as a SIGSEGV immediately after a guest toplevel is destroyed.
        window.isReleasedWhenClosed = false
        window.contentView = contentView
        if !isPopup && !serverDecorated {
            // `contentRect:` always describes the traditional area below the
            // title bar. With `.fullSizeContentView`, however, SurfaceView
            // expands across that title bar as Apple documents. Passing the
            // Wayland window geometry as `contentRect` therefore makes the
            // actual surface one title-bar taller. CSD has no separate AppKit
            // content area: its xdg window geometry is the complete frame.
            // Size the frame itself so SurfaceView is exactly the committed
            // logical size and no unpainted strip is added at the bottom.
            window.setFrame(
                NSRect(origin: window.frame.origin, size: contentSize),
                display: false)
        }
        contentView.input = self
        window.delegate = self
        window.tabbingMode = .disallowed
        // Tracking areas request motion in their bounds, and this also enables
        // the ordinary responder-chain path on AppKit versions that consult the
        // window flag first. It is false by default.
        window.acceptsMouseMovedEvents = true
        window.miniwindowImage = applicationIcon
        // xdg_toplevel commonly sends constraints before its first buffer.
        // NativeWindow exists at that point but NSWindow is materialized only
        // on the first frame, so replay the cached complete constraint state
        // before the user can begin an interactive resize.
        self.window = window
        bridge?.updateWindowPresence()
        applyConstraints()
        applyParent()

        if let popup {
            window.hasShadow = true
            window.isMovable = false
            position(window, forPopup: popup, size: contentSize)
            // Attachment is WindowServer positioning/lifetime state, not view
            // hierarchy: the panel keeps its own surface while following the
            // immediate parent (which may itself be another popup).
            bridge?.window(popup.parent)?.window?.addChildWindow(window, ordered: .above)
            window.orderFront(nil)
        } else {
            applyToplevelAppearance(to: window)
            window.title = title
            window.center()
            // Ordering front is not enough. If NativePipe is not the frontmost
            // application — and four minutes after launch it usually is not — the
            // window appears but never becomes key, so no NSEvent reaches it,
            // windowDidBecomeKey never fires, and the client is never told it has
            // focus. A window opening on the user's behalf is exactly the case
            // where activating is the right thing to do.
            if !NSApp.isActive { NSApp.activate(ignoringOtherApps: true) }
            window.makeKeyAndOrderFront(nil)
            window.makeFirstResponder(contentView)
            // xdg_toplevel state requests can precede the first buffer. The
            // NativeWindow exists then, but its NSWindow deliberately does
            // not; replay the requested state once AppKit can apply it.
            if let requestedMaximized { setMaximized(requestedMaximized) }
            if let requestedFullscreen { setFullscreen(requestedFullscreen) }
            Self.note("window \(windowID) key=\(window.isKeyWindow) firstResponder=\(String(describing: window.firstResponder))")
        }
		bridge?.registerDisplayClock(self, screen: window.screen)
		bridge?.windowScreenChanged(windowID, screen: window.screen)
    }

    private func styleMaskForToplevel() -> NSWindow.StyleMask {
        serverDecorated
            ? [.titled, .closable, .miniaturizable, .resizable]
            : [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView]
    }

    /// CSD windows keep a real AppKit frame for native resize, shadows, spaces
    /// and fullscreen, but the client's surface covers that frame completely.
    /// AppKit must not add a second visible title bar or infer a drag region —
    /// the client identifies drags explicitly with xdg_toplevel.move.
    private func applyToplevelAppearance(to window: NSWindow) {
        window.hasShadow = true
        window.isMovableByWindowBackground = false
        window.titleVisibility = serverDecorated ? .visible : .hidden
        window.titlebarAppearsTransparent = !serverDecorated
        window.titlebarSeparatorStyle = serverDecorated ? .automatic : .none
        window.standardWindowButton(.closeButton)?.isHidden = !serverDecorated
        window.standardWindowButton(.miniaturizeButton)?.isHidden = !serverDecorated
        window.standardWindowButton(.zoomButton)?.isHidden = !serverDecorated
    }

    /// Popup coordinates arrive relative to the parent's xdg window geometry,
    /// top-down. The parent's content view is exactly that cropped geometry.
    /// macOS screen coordinates run bottom-up, so the vertical axis is inverted
    /// against the parent's content rectangle rather than against the screen.
    private func position(_ window: NSWindow, forPopup popup: Popup, size: NSSize) {
        guard let parent = bridge?.window(popup.parent) else { return }
        let content = parent.surfaceRectInScreen
        window.setFrameOrigin(
            NSPoint(x: content.minX + popup.origin.x,
                    y: content.maxY - popup.origin.y - size.height))
    }

    func applyPopupGeometry(origin: CGPoint, size: NSSize) {
        guard var popup else { return }
        popup.origin = origin
        self.popup = popup
        guard let window else { return }
        window.setContentSize(size)
        position(window, forPopup: popup, size: size)
    }

    /// Visible screen bounds expressed in the parent's top-down surface-local
    /// coordinate space, which is exactly the space xdg_positioner uses.
    var popupConstraintBounds: CGRect? {
        guard let window, let screen = window.screen else { return nil }
        let parent = surfaceRectInScreen
        let visible = screen.visibleFrame
        return CGRect(
            x: visible.minX - parent.minX,
            y: parent.maxY - visible.maxY,
            width: visible.width,
            height: visible.height)
    }

    /// The Wayland surface is the content view, not AppKit's contentLayoutRect.
    /// They are identical for a normal titled window, but fullSizeContentView
    /// deliberately leaves contentLayoutRect inset by the hidden title bar.
    private var surfaceRectInScreen: NSRect {
        guard let window else { return .zero }
        let inWindow = contentView.convert(contentView.bounds, to: nil)
        return window.convertToScreen(inWindow)
    }

	func close() {
        pointerButton(.left, pressed: false)
        endScrollGesture()
        releasePressedKeys()
        pendingConfigure = nil
        contentView.clearDisplayedSurface()
        let pending = pendingPresentations
        pendingPresentations.removeAll()
        for presentation in pending {
            bridge?.send(.framePresented(
                surface: presentation.surface, presentationID: presentation.id))
        }
		bridge?.windowClosed(windowID)
		bridge?.unregisterDisplayClock(self)
        asyncScenePresenter?.cancelPending()
		bridge?.flushSceneFeedback(surface: surfaceID)
		asyncScenePresenter?.invalidateDrawableAges()
        // `orderOut` only hides a window; it does not terminate its AppKit
        // lifetime.  In particular a popup remains retained by its parent as a
        // child window, and reconnecting the compositor can then leave an old
        // generation of invisible/stale NSWindows behind.  This is an
        // authoritative guest-side destroy, so detach it and close it without
        // invoking windowShouldClose (which is only for a user's close request).
        if let window {
            if let parent = window.parent {
                parent.removeChildWindow(window)
            }
            for child in window.childWindows ?? [] {
                window.removeChildWindow(child)
            }
            window.delegate = nil
            window.close()
        }
        window = nil
        bridge?.updateWindowPresence()
    }

    // MARK: - Geometry

    /// AppKit points and Wayland surface coordinates are both logical units.
    /// Buffer scale controls attached pixel density and must never change an
    /// xdg_toplevel.configure size.
    @discardableResult
    private func sendConfigure(
		states: [Windowing.ToplevelState], force: Bool = false
	) -> Bool {
        guard window != nil else { return false }
        let size = Windowing.Size(
            width: max(1, Int(contentView.bounds.width.rounded())),
            height: max(1, Int(contentView.bounds.height.rounded())))
		guard force || size != lastConfiguredSize || states != lastConfiguredStates else {
			return false
		}

		// This is the host-side counterpart of Mutter's BEFORE_REDRAW slot: a
		// later AppKit resize sample replaces only a record that has not crossed
		// the transport yet. The display clock emits the current tuple before it
		// wakes frame-throttled clients.
		pendingConfigure = PendingConfigure(size: size, states: states, force: force)
		return true
    }

	func displayClockFired(_ displayLink: CADisplayLink) {
        if nativeMoveInProgress { finishWindowMoveIfReleased(pressedMouseButtons: NSEvent.pressedMouseButtons) }
        let period = displayLink.targetTimestamp - displayLink.timestamp
        if period.isFinite && period >= 0.001 && period <= 1 {
            displayIntervalNanoseconds = UInt32((period * 1_000_000_000).rounded())
        }
        if canPresent { bridge?.flushSceneFeedback(surface: surfaceID) }
        // Deliver the newest resize before waking a frame-throttled client, so
        // the draw started by this tick targets the newest logical size.
        flushConfigure()

        // Frame callbacks and FIFO latching are paced by the display clock.
        // Source buffers were already released by their Metal completion.
        flushPresentations()

		if presenterNeedsDisplayRetry {
			presenterNeedsDisplayRetry = false
			asyncScenePresenter?.resumeAfterDisplayTick()
		}
    }

    private func flushPresentations() {
        for presentation in pendingPresentations {
            bridge?.send(.framePresented(
                surface: presentation.surface,
                presentationID: presentation.id))
        }
        pendingPresentations.removeAll(keepingCapacity: true)
    }

    private func flushConfigure() {
        guard let pending = pendingConfigure else { return }
        pendingConfigure = nil
		guard pending.force || pending.size != lastConfiguredSize ||
		      pending.states != lastConfiguredStates else {
            return
        }
        lastConfiguredSize = pending.size
        lastConfiguredStates = pending.states
        configureSerial &+= 1
		if let anchor = pendingResizeCompletionAnchor,
		   !pending.states.contains(.resizing) {
			awaitingResizeCommitSerial = configureSerial
			resizeCommitAnchor = anchor
			pendingResizeCompletionAnchor = nil
		}
        if !pending.states.contains(.resizing) || Self.frameTrace {
            Self.note("configure window=\(windowID) -> \(pending.size.width)x\(pending.size.height)")
        }
        bridge?.send(
            .configure(
                window: windowID, size: pending.size,
                states: pending.states, serial: configureSerial))
    }

    // MARK: - Input

    /// Pointer coordinates are surface-local *logical* units — points, the same
    /// space `xdg_toplevel.configure` speaks — not buffer pixels. The content
    /// view is flipped, so its coordinates already run top-down like Wayland's.
    func pointerEntered(at point: CGPoint) {
        let point = windowPoint(from: point)
        bridge?.send(.pointerEntered(window: windowID, x: point.x, y: point.y))
    }

    func pointerMoved(to point: CGPoint) {
        if nativeMoveInProgress { finishWindowMoveIfReleased(pressedMouseButtons: NSEvent.pressedMouseButtons) }
        guard !nativeMoveInProgress else { return }
        let point = windowPoint(from: point)
        bridge?.send(.pointerMoved(window: windowID, x: point.x, y: point.y))
    }

    private func windowPoint(from contentPoint: CGPoint) -> CGPoint {
        SurfaceCoordinateSpace(windowGeometry).windowPoint(fromContent: contentPoint)
    }

    func fileDragUpdated(_ info: NSDraggingInfo, at point: CGPoint, entering: Bool) -> NSDragOperation {
        bridge?.fileDrag.destination(info, window: windowID, point: windowPoint(from: point), entering: entering) ?? []
    }
    func fileDragLeft() { bridge?.fileDrag.leave() }
    func fileDrop(_ info: NSDraggingInfo) -> Bool { bridge?.fileDrag.perform(info) ?? false }
    func beginFileDrag(view: NSView, event: NSEvent) -> Bool {
        bridge?.fileDrag.beginExportIfNeeded(view: view, event: event) ?? false
    }

    func pointerLeft() {
        endScrollGesture()
        bridge?.send(.pointerLeft(window: windowID))
    }

    func pointerButton(_ button: Windowing.PointerButton, pressed: Bool, event: NSEvent? = nil) {
        if button == .left {
            // WindowServer may consume mouse-up after performDrag. A later
            // press must never inherit the previous guest-side pointer grab.
            if pressed && leftButtonPressed {
                bridge?.send(.pointerButton(window: windowID, button: .left, pressed: false))
            }
            moveMouseDown = pressed && event?.type == .leftMouseDown ? event : nil
            nativeMoveInProgress = false
            moveHandoffTime = nil
            guard pressed || leftButtonPressed else { return }
            leftButtonPressed = pressed
        }
        // A click anywhere but inside a menu closes it, which is what makes a
        // grab feel like a grab.
        if pressed, !isPopup { bridge?.dismissPopups(ownedBy: windowID) }
        bridge?.send(.pointerButton(window: windowID, button: button, pressed: pressed))
    }

    func beginInteractiveMove() {
        guard let event = takeWindowMoveEvent(pressedMouseButtons: NSEvent.pressedMouseButtons),
              let window else { return }
        Self.note("move window=\(windowID) input_age_ms=\((ProcessInfo.processInfo.systemUptime - event.timestamp) * 1000)")
        if Self.inputTrace {
            let now = ProcessInfo.processInfo.systemUptime
            moveHandoffTime = now
            FileHandle.standardError.write(Data("[input] move window=\(windowID) down_to_handoff_ms=\((now - event.timestamp) * 1000)\n".utf8))
        }
        // This returns immediately; WindowServer owns movement, independent
        // of SSH, decoding and presentation. AppKit requires the ORIGINAL down.
        window.performDrag(with: event)
    }

    func takeWindowMoveEvent(pressedMouseButtons: Int) -> NSEvent? {
        guard !isPopup, !nativeMoveInProgress, leftButtonPressed,
              let event = moveMouseDown, event.windowNumber == window?.windowNumber else { return nil }
        moveMouseDown = nil
        guard pressedMouseButtons & 1 != 0 else {
            pointerButton(.left, pressed: false)
            return nil
        }
        nativeMoveInProgress = true
        return event
    }

    func finishWindowMoveIfReleased(pressedMouseButtons: Int) {
        guard nativeMoveInProgress, pressedMouseButtons & 1 == 0 else { return }
        // The existing local display clock observes release even if AppKit
        // sends no mouse-up. This never waits for another remote video frame.
        pointerButton(.left, pressed: false)
        if let window {
            pointerMoved(to: contentView.convert(window.mouseLocationOutsideOfEventStream, from: nil))
        }
    }

    func pointerScroll(dx: Double, dy: Double, precise: Bool) {
        scrollGestureActive = precise && (dx != 0 || dy != 0)
        bridge?.send(.pointerScroll(window: windowID, dx: dx, dy: dy, isPrecise: precise))
    }

    func endScrollGesture() {
        guard scrollGestureActive else { return }
        // A precise zero record is the existing wire representation of axis_stop.
        pointerScroll(dx: 0, dy: 0, precise: true)
    }

    func pointerScroll(with event: NSEvent, at point: CGPoint) {
        guard let bridge, bridge.acceptsKeyboardInput else { return }
        // Wayland reports finger motion and axis_stop at finger lift. Clients
        // may implement inertia; there is no inertia capability negotiation.
        // AppKit's synthesized momentum is not another finger gesture.
        guard event.momentumPhase.isEmpty else {
            endScrollGesture()
            return
        }
        let phase = event.phase
        if phase.contains(.began) || !event.hasPreciseScrollingDeltas {
            endScrollGesture()
        }
        if phase.contains(.cancelled) {
            endScrollGesture()
            return
        }
        let delta = bridge.scrollDeltas(for: event)
        if delta.dx != 0 || delta.dy != 0 {
            pointerMoved(to: point)
            pointerScroll(dx: delta.dx, dy: delta.dy, precise: event.hasPreciseScrollingDeltas)
        }
        // Zero-delta began/changed/stationary events are not gesture boundaries.
        // Preserve a final nonzero displacement before sending a separate stop.
        if phase.contains(.ended) { endScrollGesture() }
    }

    func key(_ macKeyCode: UInt16, pressed: Bool, flags: NSEvent.ModifierFlags) {
        guard let code = KeyTranslation.evdevCode(for: macKeyCode) else {
            Self.note("no evdev code for macOS key \(macKeyCode); dropped")
            return
        }
        bridge?.send(
            .key(window: windowID, keycode: code, pressed: pressed,
                 modifiers: KeyTranslation.modifiers(from: flags)))
    }

    func shortcutRule(for event: NSEvent) -> KeyboardShortcutRule? {
        guard let chord = ShortcutTranslation.chord(for: event) else { return nil }
        return bridge?.shortcutPreferences.rule(for: chord, applicationID: applicationID)
    }

    var acceptsKeyboardInput: Bool { bridge?.acceptsKeyboardInput == true }

    func releasePressedKeys() { contentView.releasePressedKeys() }

    func handleKeyUp(_ event: NSEvent) -> Bool { contentView.handleKeyUp(event) }

}

// MARK: - NSWindowDelegate

extension NativeWindow: NSWindowDelegate {
    /// AppKit owns the interactive frame. The newest logical size is sampled at
    /// the display rate, but an old client frame is never stretched to it.
    func windowDidResize(_ notification: Notification) {
        guard !isPopup else { return }
		if !applyingCommittedGeometry {
			sendConfigure(states: activeStates())
		}
        bridge?.parentGeometryChanged(windowID)
    }

    var canPresent: Bool {
        guard let window, window.isVisible, window.occlusionState.contains(.visible),
              let id = window.screen?.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else { return false }
        return CGDisplayIsActive(id.uint32Value) != 0
    }

    func windowDidChangeOcclusionState(_ notification: Notification) {
        if canPresent { bridge?.flushSceneFeedback(surface: surfaceID) }
    }

	func windowWillStartLiveResize(_ notification: Notification) {
		guard !isPopup, let window else { return }
		liveResizeStartFrame = window.frame
		pendingResizeCompletionAnchor = nil
		awaitingResizeCommitSerial = nil
		resizeCommitAnchor = nil
	}

    func windowDidMove(_ notification: Notification) {
        if let start = moveHandoffTime {
            moveHandoffTime = nil
            FileHandle.standardError.write(Data("[input] move window=\(windowID) handoff_to_did_move_ms=\((ProcessInfo.processInfo.systemUptime - start) * 1000)\n".utf8))
        }
        bridge?.parentGeometryChanged(windowID)
    }

	func windowDidChangeScreen(_ notification: Notification) {
		guard let window else { return }
		bridge?.registerDisplayClock(self, screen: window.screen)
		bridge?.windowScreenChanged(windowID, screen: window.screen)
	}

    /// The drag is over; the client should land on the exact size immediately.
    func windowDidEndLiveResize(_ notification: Notification) {
		guard let window else { return }
		let start = liveResizeStartFrame ?? window.frame
		liveResizeStartFrame = nil
		pendingResizeCompletionAnchor = WindowFrameAnchor.inferred(
			from: start, to: window.frame)
		// AppKit normally clears inLiveResize before this delegate callback, but
		// remove the state explicitly so the protocol boundary never depends on
		// callback timing. This final tuple replaces any unsent resizing tuple and
		// must receive a fresh serial even when its dimensions are unchanged.
		let finalStates = activeStates().filter { $0 != .resizing }
		_ = sendConfigure(states: finalStates, force: true)
        // The final non-resizing state and exact size should not wait for the
        // next turn after AppKit leaves its tracking loop.
        flushConfigure()
    }

    /// Dragging a window to another display changes its backing scale, which in
    /// Wayland terms is a different output scale for that surface.
    func windowDidChangeBackingProperties(_ notification: Notification) {
        guard let window else { return }
		bridge?.windowScreenChanged(windowID, screen: window.screen)
        sendConfigure(states: activeStates())
        bridge?.parentGeometryChanged(windowID)
    }

    func windowDidBecomeKey(_ notification: Notification) {
        Self.note("window \(windowID) became key")
        guard !isPopup else { return }
        bridge?.send(.keyboardFocus(window: windowID))
        sendConfigure(states: activeStates())
    }

    func windowDidResignKey(_ notification: Notification) {
        Self.note("window \(windowID) resigned key")
        releasePressedKeys()
        guard !isPopup else { return }
        // A menu whose owner lost focus has no reason to stay up.
        bridge?.dismissPopups(ownedBy: windowID)
        bridge?.send(.keyboardFocus(window: nil))
        sendConfigure(states: activeStates())
    }

    /// A close button is a *request* in Wayland. The client may refuse, or put up
    /// an "unsaved changes" dialog, so the window is not torn down here.
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        bridge?.send(isPopup ? .dismissPopup(window: windowID) : .close(window: windowID))
        return false
    }

    fileprivate func activeStates() -> [Windowing.ToplevelState] {
        guard let window else { return [] }
        var states: [Windowing.ToplevelState] = []
        if window.isKeyWindow { states.append(.activated) }
        // Clients use this to skip expensive work mid-drag.
        if window.inLiveResize { states.append(.resizing) }
        if window.isZoomed { states.append(.maximized) }
        if window.styleMask.contains(.fullScreen) { states.append(.fullscreen) }
        return states
    }
}

/// Owns every `CAMetalLayer` drawable-pool operation on one serial queue.
/// `nextDrawable()` may wait tens of milliseconds for WindowServer, so it must
/// not run on AppKit's main thread. AppKit geometry remains on the main actor,
/// while both `drawableSize` and `nextDrawable()` stay on this queue so a live
/// resize cannot mutate the pool concurrently. One latest-value pending slot
/// drops stale resize frames.
final class AsyncMetalScenePresenter: @unchecked Sendable {
    private static let frameTrace =
        ProcessInfo.processInfo.environment["NATIVEPIPE_FRAME_TRACE"] != nil

    private struct Work: @unchecked Sendable {
        let epoch: UInt64
        let scene: Windowing.SceneSnapshot
        let layers: [ResolvedSceneLayer]
        let drawableSize: CGSize
        let readComplete: @MainActor @Sendable (Bool) -> Void
        let latches: [@MainActor @Sendable () -> Void]
        let presented: (@MainActor @Sendable (Bool) -> Void)?

        func superseding(_ older: Work) -> Work {
            Work(epoch: epoch, scene: scene.includingUnrenderedDamage(from: older.scene),
                 layers: layers, drawableSize: drawableSize, readComplete: readComplete,
                 latches: older.latches + latches, presented: presented)
        }
    }

    private struct CaptureRequest: @unchecked Sendable {
        let id: UInt64
        let completion: @MainActor @Sendable (Result<RenderedFrameCapture, Error>) -> Void
    }

	private let layer: CAMetalLayer
    private let renderer: HostSceneRenderer
    private let requestDisplayRetry: (@Sendable () -> Void)?
    private let queue = DispatchQueue(
        label: "com.nativepipe.metal-present", qos: .userInteractive)
	private let lock = NSLock()
    private var pending: Work?
	private var epoch: UInt64 = 0
	private var drainScheduled = false
	private var appliedDrawableSize = CGSize.zero
	private var drawableAges: DrawableAgeTracker
    private var captureRequests: [CaptureRequest] = []
    private var nextCaptureID: UInt64 = 0

    private enum ProcessResult: Equatable {
        case handled
        case retryAfterDisplay
    }

    private enum RetryDisposition {
        case scheduled
        case superseded
        case cancelled
    }

    init(
        layer: CAMetalLayer, device: MTLDevice, renderer: HostSceneRenderer,
		requestDisplayRetry: (@Sendable () -> Void)? = nil
	) {
        self.layer = layer
        self.renderer = renderer
		self.requestDisplayRetry = requestDisplayRetry
		self.drawableAges = DrawableAgeTracker(capacity: layer.maximumDrawableCount)
        layer.device = device
		layer.framebufferOnly = false
    }

	func resumeAfterDisplayTick() {
		lock.lock()
		let shouldSchedule = pending != nil && !drainScheduled
		if shouldSchedule { drainScheduled = true }
		lock.unlock()
		if shouldSchedule { queue.async { self.drain() } }
	}

	func invalidateDrawableAges() {
		queue.async { self.drawableAges.invalidate() }
	}

    func captureNextFrame(
        completion: @escaping @MainActor @Sendable (Result<RenderedFrameCapture, Error>) -> Void
    ) -> UInt64 {
        lock.lock()
        nextCaptureID &+= 1
        let id = nextCaptureID
        captureRequests.append(CaptureRequest(id: id, completion: completion))
        lock.unlock()
        return id
    }

    func cancelCapture(_ id: UInt64) {
        lock.lock()
        captureRequests.removeAll { $0.id == id }
        lock.unlock()
    }

    func enqueue(
        scene: Windowing.SceneSnapshot, layers: [ResolvedSceneLayer],
        drawableSize: CGSize,
        readComplete: @escaping @MainActor @Sendable (Bool) -> Void,
        latched: @escaping @MainActor @Sendable () -> Void,
        presented: (@MainActor @Sendable (Bool) -> Void)? = nil
    ) {
        lock.lock()
        let superseded = pending
		let work = superseded.map {
			Work(
				epoch: epoch,
				scene: scene.includingUnrenderedDamage(from: $0.scene),
				layers: layers, drawableSize: drawableSize,
				readComplete: readComplete,
				latches: $0.latches + [latched], presented: presented)
		} ?? Work(
			epoch: epoch, scene: scene, layers: layers,
			drawableSize: drawableSize, readComplete: readComplete,
			latches: [latched], presented: presented)
        pending = work
        let shouldSchedule = !drainScheduled
        if shouldSchedule { drainScheduled = true }
        lock.unlock()

        // This work never reached Metal. Releasing it immediately is what
        // makes the slot latest-value: retaining every stale resize scene
        // would pin the client's whole swapchain until drawableSize settles.
        if let superseded {
            finish(superseded, success: false)
        }
        if shouldSchedule { queue.async { self.drain() } }
    }

    func cancelPending() {
        lock.lock()
        epoch &+= 1
        let cancelled = pending
        pending = nil
        let captures = captureRequests
        captureRequests.removeAll(keepingCapacity: true)
        lock.unlock()
        if let work = cancelled {
            finish(work, success: false)
            latch(work)
        }
        completeCaptures(captures, result: .failure(
            ComputerUseWindowError.captureFailed))
    }

    private func takeNext() -> Work? {
        lock.lock()
        defer { lock.unlock() }
        guard let work = pending else {
			drainScheduled = false
			return nil
		}
        pending = nil
        return work
    }

    private func drain() {
        while let work = takeNext() {
            let result = autoreleasepool { process(work) }
            guard result == .retryAfterDisplay else { continue }
            switch scheduleRetry(work) {
            case .scheduled:
                // Do not spin through stale drawables from the old pool. A
                // later queue turn gives CoreAnimation time to retire them.
                return
            case .superseded:
                // A newer scene owns the slot and inherited the output latch.
                finish(work, success: false)
            case .cancelled:
                // The window generation disappeared while nextDrawable was
                // waiting. Complete its callbacks so a remap starts cleanly.
                finish(work, success: false)
                latch(work)
            }
        }
    }

    private func process(_ original: Work) -> ProcessResult {
        var work = original
        guard isCurrent(work) else {
            finish(work, success: false)
            latch(work)
            return .handled
        }
        if appliedDrawableSize != work.drawableSize {
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            layer.drawableSize = work.drawableSize
            CATransaction.commit()
            appliedDrawableSize = work.drawableSize
            drawableAges.invalidate()
            if Self.frameTrace {
                let message =
                    "[nsw] drawable capacity -> \(Int(work.drawableSize.width))x" +
                    "\(Int(work.drawableSize.height))\n"
                FileHandle.standardError.write(Data(message.utf8))
            }
        }
        let drawableStart = ProcessInfo.processInfo.systemUptime
        guard let drawable = layer.nextDrawable() else {
            if Self.frameTrace {
                let elapsed =
                    (ProcessInfo.processInfo.systemUptime - drawableStart) * 1_000
                let message = String(
                    format: "[nsw] nextDrawable nil after %.2f ms\n", elapsed)
                FileHandle.standardError.write(Data(message.utf8))
            }
            return .retryAfterDisplay
        }
        if Self.frameTrace {
            let elapsed =
                (ProcessInfo.processInfo.systemUptime - drawableStart) * 1_000
            if elapsed >= 2 {
                let message = String(
                    format: "[nsw] nextDrawable waited %.2f ms\n", elapsed)
                FileHandle.standardError.write(Data(message.utf8))
            }
        }
        lock.lock()
        guard work.epoch == epoch else {
            lock.unlock()
            finish(work, success: false)
            latch(work)
            return .handled
        }
        if let newer = pending {
            // Resize needs a drawable from the new pool; the existing retry
            // path transfers damage and latch obligations to the newer work.
            guard newer.drawableSize == work.drawableSize else {
                lock.unlock()
                return .retryAfterDisplay
            }
            pending = nil
            let previous = work
            work = newer.superseding(previous)
            lock.unlock()
            finish(previous, success: false)
        } else {
            lock.unlock()
        }
        let selected = work
        let scene = selected.scene
        if let presented = work.presented {
            drawable.addPresentedHandler { drawable in
                let displayed = drawable.presentedTime > 0
                MainRunLoop.perform { presented(displayed) }
            }
        }
		let captures = takeCaptureRequests()
		let plan = drawableAges.plan(
			drawableID: drawable.texture.gpuResourceID._impl,
			scene: scene, drawableWidth: drawable.texture.width,
			drawableHeight: drawable.texture.height)
        do {
            try renderer.encode(
                scene: scene, layers: work.layers,
				damage: plan.damage, redrawAll: plan.redrawAll,
				drawable: drawable, capture: !captures.isEmpty
            ) { command, captured in
                if command.status != .completed, let error = command.error {
                    FileHandle.standardError.write(
                        Data("[nsw] Metal scene failed: \(error)\n".utf8))
                }
				if command.status != .completed {
					self.queue.async { self.drawableAges.invalidate() }
				}
                if !captures.isEmpty {
                    let result: Result<RenderedFrameCapture, Error> = captured.map {
                        .success($0)
                    } ?? .failure(ComputerUseWindowError.captureFailed)
                    self.completeCaptures(captures, result: result)
                }
                self.finish(selected, success: command.status == .completed)
            }
			drawableAges.commit(plan)
            // A drawable has accepted this scene in FIFO order. Queue its
            // Wayland callback for the next display-link tick. Source-buffer
            // release remains tied to Metal completion above.
            latch(work)
        } catch {
            FileHandle.standardError.write(
                Data("[nsw] could not encode Metal scene: \(error)\n".utf8))
			finish(work, success: false)
			latch(work)
            completeCaptures(captures, result: .failure(error))
			return .handled
        }
        return .handled
    }

    private func isCurrent(_ work: Work) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return work.epoch == epoch
    }

    private func takeCaptureRequests() -> [CaptureRequest] {
        lock.lock()
        defer { lock.unlock() }
        let requests = captureRequests
        captureRequests.removeAll(keepingCapacity: true)
        return requests
    }

    private func completeCaptures(
        _ requests: [CaptureRequest],
        result: Result<RenderedFrameCapture, Error>
    ) {
        guard !requests.isEmpty else { return }
        MainRunLoop.perform {
            for request in requests { request.completion(result) }
        }
    }

    /// Retry only when no newer scene is already waiting. A drawable-pool miss
    /// sleeps until the shared display clock; a new scene wakes it immediately.
    private func scheduleRetry(_ work: Work) -> RetryDisposition {
        lock.lock()
        let disposition: RetryDisposition
        if work.epoch != epoch {
            disposition = .cancelled
        } else if let newer = pending {
            pending = Work(
                epoch: newer.epoch,
                scene: newer.scene.includingUnrenderedDamage(from: work.scene),
                layers: newer.layers, drawableSize: newer.drawableSize,
                readComplete: newer.readComplete,
                latches: work.latches + newer.latches,
                presented: newer.presented)
            disposition = .superseded
        } else {
            pending = work
			drainScheduled = false
            disposition = .scheduled
        }
        lock.unlock()
        guard case .scheduled = disposition else { return disposition }
		if let requestDisplayRetry {
			requestDisplayRetry()
		} else {
			queue.asyncAfter(deadline: .now() + .milliseconds(16)) {
				self.drain()
			}
		}
		return disposition
    }

    private func finish(_ work: Work, success: Bool) {
        let readComplete = work.readComplete
        MainRunLoop.perform { readComplete(success) }
    }

    private func latch(_ work: Work) {
        let latches = work.latches
        MainRunLoop.perform {
            for latch in latches { latch() }
        }
    }
}

// MARK: - Content view

/// Hosts either the direct CAMetalDrawable path or the remote decoded surface.
private final class SurfaceView: NSView {
    /// Set once the window exists; events before that have nowhere to go.
    weak var input: NativeWindow?

    private var trackingArea: NSTrackingArea?
    private var keyboard = KeyboardState()

    // MARK: Text input state
    //
    /// True while a guest client has an enabled zwp_text_input_v3. Only then is
    /// the key event offered to the macOS input context first — otherwise every
    /// keystroke would be routed through an IME that no client asked for.
    var textInputEnabled = false
    /// The caret, in surface-local logical points, for placing the candidate
    /// window. Zero means the client has not said, and the view's origin is used.
    var textCursorRect: CGRect = .zero
    private var markedText = ""
    /// Set for the duration of one keyDown and cleared by whichever
    /// NSTextInputClient callback consumes it. If it survives, nothing did, and
    /// the key goes to the guest as an ordinary key press.
    private var unconsumedKeyEvent: NSEvent?
    /// The legacy CPU/remote surface currently on screen, held for exactly as
    /// long as it is installed in the layer. GPU windows use `metalLayer`.
    private var displayed: IOSurfaceRef?
    private var displayedOwner: AnyObject?
    /// Pixel density of the most recently committed Wayland scene. The Metal
    /// layer follows the AppKit view, while HostSceneRenderer keeps older scene
    /// pixels 1:1 at the drawable's top-left and clips them to the current view.
    private var metalScale: CGFloat = 1
    /// Holds client content in AppKit point space without rubber-band scaling.
    /// If AppKit is ahead of the client during resize, the old scene remains at
    /// its committed size and is clipped (or leaves an unpainted edge) until an
    /// exact-size frame arrives.
    private let sceneLayer = CALayer()
    /// Pixel contents of the root wl_surface. The NSView backing layer is only
    /// the xdg window-geometry clip and never carries surface pixels itself.
    let surfaceLayer = CALayer()
    /// Streaming GPU content uses Core Animation's drawable pool instead of
    /// mutating an IOSurface stored in `CALayer.contents`.
    let metalLayer = CAMetalLayer()

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        let promisedTypes = NSFilePromiseReceiver.readableDraggedTypes.map { NSPasteboard.PasteboardType($0) }
        registerForDraggedTypes([.fileURL] + promisedTypes)
        sceneLayer.anchorPoint = .zero
        sceneLayer.isGeometryFlipped = true
        surfaceLayer.contentsGravity = .resize
        surfaceLayer.isOpaque = false
        surfaceLayer.anchorPoint = .zero
        surfaceLayer.isGeometryFlipped = true
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = false
        metalLayer.maximumDrawableCount = 3
        metalLayer.allowsNextDrawableTimeout = true
        // SurfaceView is flipped. AppKit maps its visual `.topLeft` placement
        // to Core Animation's `.bottomLeft` gravity (verified against the
        // layerContentsPlacement API), so the standalone Metal child must use
        // the same gravity instead of the oppositely oriented `.topLeft`.
        metalLayer.contentsGravity = .bottomLeft
        metalLayer.isOpaque = false
        metalLayer.anchorPoint = .zero
        metalLayer.isGeometryFlipped = true
        metalLayer.isHidden = true
        wantsLayer = true
        layerContentsRedrawPolicy = .never
        layerContentsPlacement = .topLeft
    }

    override func makeBackingLayer() -> CALayer {
        let layer = CALayer()
        layer.isOpaque = false
        layer.masksToBounds = true
        sceneLayer.addSublayer(surfaceLayer)
        sceneLayer.addSublayer(metalLayer)
        layer.addSublayer(sceneLayer)
        return layer
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("not supported") }

    override var isFlipped: Bool { true }

    override var acceptsFirstResponder: Bool { true }

    override func resetCursorRects() {
        super.resetCursorRects()
        addCursorRect(bounds, cursor: input?.pointerCursor ?? .arrow)
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        layoutDisplayedSurface()
    }

    /// Even underneath a transparent AppKit title bar, mouse input belongs to
    /// the Wayland client. GTK/Qt requests an actual window drag explicitly.
    override var mouseDownCanMoveWindow: Bool { false }

    /// Clicking into an unfocused window should reach the client, not be eaten
    /// by the activation itself — that is what users expect of a native window.
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let trackingArea { removeTrackingArea(trackingArea) }
        let area = NSTrackingArea(
            rect: bounds,
            // A non-activating menu panel intentionally never becomes the key
            // window. activeInKeyWindow therefore delivered clicks but no
            // motion, leaving GTK's highlighted menu item frozen. Pointer focus
            // is independent of keyboard focus, so every window in the active
            // app must track motion.
            options: [.mouseEnteredAndExited, .mouseMoved, .activeInActiveApp, .inVisibleRect],
            owner: self,
            userInfo: nil)
        addTrackingArea(area)
        trackingArea = area
    }

    private func location(of event: NSEvent) -> CGPoint {
        convert(event.locationInWindow, from: nil)
    }

    // MARK: Pointer

    override func mouseEntered(with event: NSEvent) {
        input?.pointerEntered(at: location(of: event))
    }

    override func mouseExited(with event: NSEvent) {
        input?.pointerLeft()
    }

    override func mouseMoved(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
    }

    override func mouseDragged(with event: NSEvent) {
        if input?.beginFileDrag(view: self, event: event) == true { return }
        input?.pointerMoved(to: location(of: event))
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        input?.fileDragUpdated(sender, at: convert(sender.draggingLocation, from: nil), entering: true) ?? []
    }
    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        input?.fileDragUpdated(sender, at: convert(sender.draggingLocation, from: nil), entering: false) ?? []
    }
    override func draggingExited(_ sender: NSDraggingInfo?) { input?.fileDragLeft() }
    override func prepareForDragOperation(_ sender: NSDraggingInfo) -> Bool { true }
    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool { input?.fileDrop(sender) ?? false }

    override func rightMouseDragged(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
    }

    override func otherMouseDragged(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
    }

    override func mouseDown(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
        input?.pointerButton(.left, pressed: true, event: event)
    }

    override func mouseUp(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
        input?.pointerButton(.left, pressed: false)
    }

    override func rightMouseDown(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
        input?.pointerButton(.right, pressed: true)
    }

    override func rightMouseUp(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
        input?.pointerButton(.right, pressed: false)
    }

    override func otherMouseDown(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
        input?.pointerButton(.middle, pressed: true)
    }

    override func otherMouseUp(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
        input?.pointerButton(.middle, pressed: false)
    }

    override func scrollWheel(with event: NSEvent) {
        input?.pointerScroll(with: event, at: location(of: event))
    }

    // MARK: Keyboard

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        // Only the guest content responder may consume the host menu shortcut;
        // native sheets and controls must retain their usual AppKit behavior.
        guard window?.firstResponder === self else { return false }
        let quit = ShortcutTranslation.chord(for: event) == ShortcutChord(.q, .logo)
        guard quit || keyboard.ownsPress(event.keyCode) || input?.shortcutRule(for: event) != nil else {
            return false
        }
        // Cmd+Q is reserved for this guest even with remapping disabled, while
        // paused, or when its rule has been deleted. Never fall through to Quit
        // VMHost. Explicitly choosing Quit from the host menu still works.
        keyDown(with: event)
        return true
    }

    override func keyDown(with event: NSEvent) {
        // Not calling super: NSResponder's default is to beep at anything it does
        // not recognise, and the client is the one deciding what a key means.
        guard input?.acceptsKeyboardInput == true else { return }
        if keyboard.ownsPress(event.keyCode) ||
            (!keyboard.ownsTextPress(event.keyCode) && input?.shortcutRule(for: event)?.target != nil) {
            forwardPress(event)
            return
        }
        if keyboard.shouldInterpret(event, textInputEnabled: textInputEnabled) {
            // The input method gets first refusal. What it takes comes back as
            // text; what it declines — Return, Escape, the arrow keys, anything
            // it routes through doCommandBySelector — is an editing command and
            // still belongs on the raw key path.
            unconsumedKeyEvent = event
            _ = inputContext?.handleEvent(event)
            guard let survived = unconsumedKeyEvent else {
                keyboard.textHandled(event)
                return
            }
            unconsumedKeyEvent = nil
            forwardPress(survived)
            return
        }
        forwardPress(event)
    }

    private func forwardPress(_ event: NSEvent) {
        guard !event.isARepeat else { return }
        let target = input?.shortcutRule(for: event)?.target
        let code = target.flatMap { ShortcutTranslation.keyCode(for: $0.key, source: event) }
        // An unavailable target layout key must not be replaced by a different
        // physical key. Pass through instead; rule editing remains reversible.
        sendKeys(keyboard.press(event, mappedCode: code,
                               mappedFlags: code == nil ? nil : target.map { ShortcutTranslation.flags(from: $0.modifiers) }))
    }

    private func sendKeys(_ keys: [KeyboardState.Key]) {
        for key in keys { input?.key(key.code, pressed: key.pressed, flags: key.flags) }
    }

    override func keyUp(with event: NSEvent) {
        _ = handleKeyUp(event)
    }

    /// Called both by normal responder dispatch and by the bridge's local
    /// monitor: NSApplication can swallow keyUp while Command is held.
    func handleKeyUp(_ event: NSEvent) -> Bool {
        let owned = keyboard.ownsPress(event.keyCode)
        sendKeys(keyboard.release(event))
        return owned
    }

    /// Modifiers arrive as a flags snapshot rather than as key events, so the
    /// press and release edges have to be recovered by comparing with the last.
    override func flagsChanged(with event: NSEvent) {
        guard input?.acceptsKeyboardInput == true else { return }
        sendKeys(keyboard.modifiersChanged(event))
    }

    override func resignFirstResponder() -> Bool {
        releasePressedKeys()
        return super.resignFirstResponder()
    }

    func releasePressedKeys() {
        unconsumedKeyEvent = nil
        sendKeys(keyboard.releaseAll())
        abandonComposition()
    }

    func configureMetalLayer(scene: Windowing.SceneSnapshot) -> CGSize? {
        displayed = nil
        displayedOwner = nil
        guard layer != nil, scene.width > 0, scene.height > 0 else { return nil }
		metalScale = CGFloat(max(scene.scale, 1))

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        sceneLayer.bounds = CGRect(origin: .zero, size: bounds.size)
        sceneLayer.position = .zero
        sceneLayer.setAffineTransform(.identity)
		let drawableSize = layoutMetalLayer()
        surfaceLayer.isHidden = true
        surfaceLayer.contents = nil
        metalLayer.isHidden = false
        CATransaction.commit()
		return drawableSize
    }

    /// Remote decoders may allocate surfaces larger than the active frame.
    /// Normalize against the allocation so unused capacity is never stretched.
    func displayCPU(
        _ surface: IOSurfaceRef, frame: Windowing.Frame,
        geometry: Windowing.Rect, owner: AnyObject?
    ) {
        displayed = surface
        displayedOwner = owner
        guard layer != nil else { return }

        let logical = CGRect(origin: .zero, size: frame.appKitPointSize)
        let contentsRect = frame.contentsRect(
            for: logical,
            allocationSize: CGSize(
                width: IOSurfaceGetWidth(surface),
                height: IOSurfaceGetHeight(surface)))

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        sceneLayer.bounds = CGRect(origin: .zero, size: bounds.size)
        sceneLayer.position = .zero
        sceneLayer.setAffineTransform(.identity)
        surfaceLayer.bounds = logical
		alignToTopLeft(surfaceLayer)
        surfaceLayer.contentsScale = frame.pixelDensity(for: logical)
        surfaceLayer.contentsRect = contentsRect
        surfaceLayer.contents = surface
        surfaceLayer.isHidden = false
        metalLayer.isHidden = true
        CATransaction.commit()
    }

    func clearDisplayedSurface() {
        displayed = nil
        displayedOwner = nil
        guard layer != nil else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        surfaceLayer.contents = nil
        surfaceLayer.isHidden = true
        metalLayer.isHidden = true
        CATransaction.commit()
    }

    func traceLayerGeometry(windowID: UInt32) {
        guard let backing = layer else { return }
        let scenePresentation = sceneLayer.presentation() ?? sceneLayer
        let surfacePresentation = surfaceLayer.presentation() ?? surfaceLayer
        let metalPresentation = metalLayer.presentation() ?? metalLayer
        var message = "[nsw] layers window=\(windowID) view=\(bounds) backing=\(backing.bounds) "
        message += "sceneBounds=\(sceneLayer.bounds) sceneFrame=\(sceneLayer.frame) "
        message += "scenePresented=\(scenePresentation.frame) surfaceBounds=\(surfaceLayer.bounds) "
        message += "surfaceFrame=\(surfaceLayer.frame) "
        message += "surfacePresented=\(surfacePresentation.frame) "
        message += "metalBounds=\(metalLayer.bounds) metalFrame=\(metalLayer.frame) "
        message += "metalPosition=\(metalLayer.position) drawable=\(metalLayer.drawableSize) "
        message += "metalPresented=\(metalPresentation.frame)\n"
        FileHandle.standardError.write(Data(message.utf8))
    }

    /// Keep the scene container aligned with the view without resizing the
    /// committed client content inside it.
    private func layoutDisplayedSurface() {
        guard layer != nil else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        sceneLayer.bounds = CGRect(origin: .zero, size: bounds.size)
        sceneLayer.position = .zero
        sceneLayer.setAffineTransform(.identity)
		alignToTopLeft(surfaceLayer)
		_ = layoutMetalLayer()
        CATransaction.commit()
    }

	/// AppKit owns the provisional live-resize frame. Keep the Metal drawable
	/// coupled to that frame as Apple recommends. HostSceneRenderer uses the
	/// committed scene extent as its drawing limit, so this does not stretch an
	/// older Wayland scene: extra pixels remain transparent and shrinking clips.
	private func layoutMetalLayer() -> CGSize {
		metalLayer.bounds = CGRect(origin: .zero, size: bounds.size)
		alignToTopLeft(metalLayer)
		metalLayer.contentsScale = metalScale
		return CGSize(
			width: max(1, (bounds.width * metalScale).rounded()),
			height: max(1, (bounds.height * metalScale).rounded()))
	}

	private func alignToTopLeft(_ child: CALayer) {
		child.position = SurfaceLayerPlacement.topLeftPosition(
			container: sceneLayer.bounds.size, child: child.bounds.size)
	}
}

// MARK: - Text input

extension NativeWindow {
    /// A guest client turned zwp_text_input_v3 on or off for this window.
    func setTextInput(enabled: Bool) {
        guard contentView.textInputEnabled != enabled else { return }
        contentView.textInputEnabled = enabled
        if !enabled { contentView.abandonComposition() }
        // The input context caches whether the responder wants text. Without
        // this it keeps the previous answer until focus moves, so the first
        // field a user clicks into gets no IME.
        NSTextInputContext.current?.invalidateCharacterCoordinates()
        window?.makeFirstResponder(contentView)
    }

    func setTextCursorRect(_ rect: CGRect) {
        contentView.textCursorRect = rect
    }

    func commitText(_ text: String) {
        bridge?.send(.textCommit(window: windowID, text: text))
    }

    func setPreedit(_ text: String, cursorBegin: Int, cursorEnd: Int) {
        bridge?.send(.textPreedit(
            window: windowID, text: text,
            cursorBegin: cursorBegin, cursorEnd: cursorEnd))
    }

    func deleteSurrounding(before: UInt32, after: UInt32) {
        bridge?.send(.textDeleteSurrounding(
            window: windowID, beforeLength: before, afterLength: after))
    }
}

/// The macOS side of the IME seam.
///
/// AppKit composes here exactly as it would for a native text view: the same
/// input methods, the same candidate window, the same key handling. What is
/// different is only where the result goes — a Wayland client in the guest
/// rather than an NSTextStorage. That is the whole point of using
/// zwp_text_input_v3 instead of forwarding keystrokes and hoping the client has
/// its own input method.
// NSTextInputClient predates Swift concurrency annotations. Keep the
// conformance usable by the Swift 6.0 toolchain declared in Package.swift,
// while dynamically enforcing the main-actor requirement at each callback.
@MainActor
extension SurfaceView: @preconcurrency NSTextInputClient {
    func insertText(_ string: Any, replacementRange: NSRange) {
        unconsumedKeyEvent = nil
        let text = (string as? NSAttributedString)?.string ?? (string as? String) ?? ""
        guard !text.isEmpty else { return }
        markedText = ""
        input?.commitText(text)
    }

    override func doCommand(by selector: Selector) {
        // Deliberately does nothing and leaves `unconsumedKeyEvent` set. This is
        // how Return, Tab, Escape and the arrows get back onto the raw key path:
        // they are commands for the client's editor, not text the IME produced.
    }

    func setMarkedText(_ string: Any, selectedRange: NSRange, replacementRange: NSRange) {
        unconsumedKeyEvent = nil
        let text = (string as? NSAttributedString)?.string ?? (string as? String) ?? ""
        markedText = text
        // text-input-v3 measures the cursor in bytes of the UTF-8 preedit, while
        // AppKit's range is in UTF-16 units. Converting through the string is
        // what keeps a CJK preedit's caret in the right place.
        let begin = utf8Offset(in: text, utf16Offset: selectedRange.location)
        let end = utf8Offset(in: text, utf16Offset: selectedRange.location + selectedRange.length)
        input?.setPreedit(text, cursorBegin: begin, cursorEnd: end)
    }

    private func utf8Offset(in text: String, utf16Offset: Int) -> Int {
        guard utf16Offset > 0 else { return 0 }
        guard let index = String.Index(
            String.UTF16View.Index(utf16Offset: utf16Offset, in: text), within: text)
        else { return text.utf8.count }
        return text.utf8.distance(from: text.utf8.startIndex, to: index.samePosition(in: text.utf8)!)
    }

    /// Drops any composition in progress without committing it.
    func abandonComposition() {
        guard !markedText.isEmpty else { return }
        markedText = ""
        inputContext?.discardMarkedText()
        input?.setPreedit("", cursorBegin: 0, cursorEnd: 0)
    }

    func unmarkText() {
        guard !markedText.isEmpty else { return }
        markedText = ""
        input?.setPreedit("", cursorBegin: 0, cursorEnd: 0)
    }

    func selectedRange() -> NSRange {
        // The client owns the document; the host has no index into it. Reporting
        // an empty selection at the caret is accurate for the only thing AppKit
        // uses it for here, which is deciding a composition is a fresh one.
        NSRange(location: 0, length: 0)
    }

    func markedRange() -> NSRange {
        markedText.isEmpty
            ? NSRange(location: NSNotFound, length: 0)
            : NSRange(location: 0, length: markedText.utf16.count)
    }

    func hasMarkedText() -> Bool { !markedText.isEmpty }

    func attributedSubstring(
        forProposedRange range: NSRange, actualRange: NSRangePointer?
    ) -> NSAttributedString? {
        // Reconversion would need the client's text, which arrives as
        // textInputSurroundingText. v1 declines rather than answering wrongly:
        // a wrong substring makes an IME replace text the user did not select.
        nil
    }

    func validAttributesForMarkedText() -> [NSAttributedString.Key] { [] }

    /// Where the candidate window goes. The rectangle the client gave is in
    /// surface-local logical points; AppKit wants screen coordinates.
    func firstRect(
        forCharacterRange range: NSRange, actualRange: NSRangePointer?
    ) -> NSRect {
        let local = textCursorRect == .zero
            ? CGRect(x: 0, y: bounds.height, width: 1, height: 16)
            : textCursorRect
        let inWindow = convert(local, to: nil)
        return window?.convertToScreen(inWindow) ?? inWindow
    }

    func characterIndex(for point: NSPoint) -> Int { NSNotFound }
}
