import AppKit
@preconcurrency import IOSurface
@preconcurrency import Metal
import NativePipeProtocol
import QuartzCore

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

    private static func note(_ message: @autoclosure () -> String) {
        guard trace else { return }
        FileHandle.standardError.write(Data("[nsw] \(message())\n".utf8))
    }

    let windowID: UInt32
    let surfaceID: UInt32

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

    private var appID: String?
    /// This is a value supplied by the guest compositor, not a host policy.
    /// Client-side is the safe construction default: it prevents an NSWindow
    /// titlebar from flashing around the first CSD frame before the protocol
    /// event arrives. Qt and other SSD clients explicitly request server-side.
    private var serverDecorated = false
    private var minimumConstraint: Windowing.Size?
    private var maximumConstraint: Windowing.Size?
    private var lastConfiguredSize: Windowing.Size?
    private var lastConfiguredStates: [Windowing.ToplevelState] = []
    private var configureSerial: UInt32 = 0

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
    }
    private var pendingConfigure: PendingConfigure?
    private var configureDisplayLink: CADisplayLink?
    /// CPU/remote contents installed since the previous display tick. GPU
    /// frames use the CAMetalDrawable's actual presentation callback instead.
    private struct Presentation: Hashable {
        let surface: UInt32
        let id: UInt32
    }
    private var pendingPresentations: [Presentation] = []
    private let metalDevice = MTLCreateSystemDefaultDevice()
    private lazy var sceneRenderer: HostSceneRenderer? = {
        guard let metalDevice else { return nil }
        return try? HostSceneRenderer(device: metalDevice)
    }()
    private lazy var asyncScenePresenter: AsyncMetalScenePresenter? = {
        guard let renderer = sceneRenderer else { return nil }
        return AsyncMetalScenePresenter(layer: contentView.metalLayer, renderer: renderer)
    }()

    init(windowID: UInt32, surfaceID: UInt32, bridge: WindowBridge, popup: Popup? = nil) {
        self.windowID = windowID
        self.surfaceID = surfaceID
        self.bridge = bridge
        self.popup = popup
        super.init()
    }

    var isPopup: Bool { popup != nil }

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
        // user. Dock identity will hang off this once the launcher exists.
        appID = value
    }

    func setServerDecorated(_ enabled: Bool) {
        guard serverDecorated != enabled else { return }
        serverDecorated = enabled
        guard let window, !isPopup else { return }
        let contentRect = window.contentRect(forFrameRect: window.frame)
        window.styleMask = styleMaskForToplevel()
        applyToplevelAppearance(to: window)
        window.setFrame(window.frameRect(forContentRect: contentRect), display: true)
        window.title = enabled ? title : ""
    }

    func setConstraints(minimum: Windowing.Size?, maximum: Windowing.Size?) {
        minimumConstraint = minimum
        maximumConstraint = maximum
        applyConstraints()
    }

    private func applyConstraints() {
        guard let window else { return }
        window.contentMinSize = minimumConstraint.map {
            NSSize(width: $0.width, height: $0.height)
        } ?? .zero
        window.contentMaxSize = maximumConstraint.map {
            NSSize(width: $0.width, height: $0.height)
        } ?? NSSize(width: 10_000_000, height: 10_000_000)
    }

    func setParent(_ parent: NativeWindow?) {
        guard let window else { return }
        if let existing = window.parent {
            existing.removeChildWindow(window)
        }
        parent?.window?.addChildWindow(window, ordered: .above)
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

    func present(frame: Windowing.Frame, surface: IOSurfaceRef) {
        prepareWindow(for: frame)
        let incoming = frame.presentationID == 0 ? nil : Presentation(
            surface: surfaceID, id: frame.presentationID)
        contentView.displayCPU(
            surface, frame: frame, geometry: windowGeometry)
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
    func present(
        scene: Windowing.SceneSnapshot, layers: [ResolvedSceneLayer],
        readComplete: @escaping @MainActor (Bool) -> Void
    ) {
        prepareWindow(for: scene)
        guard let device = metalDevice,
              let presenter = asyncScenePresenter,
              contentView.configureMetalLayer(
                device: device, scene: scene, geometry: windowGeometry)
        else {
            Self.note("could not configure Metal scene window=\(windowID)")
            readComplete(false)
            awaitPresentation(
                surface: scene.surface, presentationID: scene.presentationID)
            return
        }
        presenter.enqueue(
            scene: scene, layers: layers, readComplete: readComplete,
            latched: { [weak self] in
                self?.bridge?.send(.framePresented(
                    surface: scene.surface,
                    presentationID: scene.presentationID))
            })
    }

    func awaitPresentation(surface: UInt32, presentationID: UInt32) {
        guard presentationID != 0 else { return }
        awaitPresentation(Presentation(surface: surface, id: presentationID))
    }

    private func awaitPresentation(_ presentation: Presentation) {
        guard !pendingPresentations.contains(presentation) else { return }
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
        if window == nil { makeWindow(contentSize: pointSize) }
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
        contentView.input = self
        window.delegate = self
        window.tabbingMode = .disallowed
        // Tracking areas request motion in their bounds, and this also enables
        // the ordinary responder-chain path on AppKit versions that consult the
        // window flag first. It is false by default.
        window.acceptsMouseMovedEvents = true
        // xdg_toplevel commonly sends constraints before its first buffer.
        // NativeWindow exists at that point but NSWindow is materialized only
        // on the first frame, so replay the cached complete constraint state
        // before the user can begin an interactive resize.
        self.window = window
        applyConstraints()

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
            window.title = serverDecorated ? title : ""
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
            Self.note("window \(windowID) key=\(window.isKeyWindow) firstResponder=\(String(describing: window.firstResponder))")
        }
        let displayLink = contentView.displayLink(
            target: self, selector: #selector(configureDisplayLinkFired(_:)))
        // Keep the link active while the window exists. Repeatedly pausing and
        // restarting it made sparse content updates degrade to a few callbacks
        // per second and detached frame pacing from the display clock.
        displayLink.isPaused = false
        displayLink.add(to: .main, forMode: .common)
        configureDisplayLink = displayLink

        // The client needs the display's scale before it can pick a buffer size,
        // and it has only ever been told on change until now.
        bridge?.send(.scaleChanged(window: windowID, scale: Int(window.backingScaleFactor)))
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

    /// The Wayland surface is the content view, not AppKit's contentLayoutRect.
    /// They are identical for a normal titled window, but fullSizeContentView
    /// deliberately leaves contentLayoutRect inset by the hidden title bar.
    private var surfaceRectInScreen: NSRect {
        guard let window else { return .zero }
        let inWindow = contentView.convert(contentView.bounds, to: nil)
        return window.convertToScreen(inWindow)
    }

    func close() {
        pendingConfigure = nil
        contentView.clearDisplayedSurface()
        let pending = pendingPresentations
        pendingPresentations.removeAll()
        for presentation in pending {
            bridge?.send(.framePresented(
                surface: presentation.surface, presentationID: presentation.id))
        }
        configureDisplayLink?.invalidate()
        configureDisplayLink = nil
        asyncScenePresenter?.cancelPending()
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
    }

    // MARK: - Geometry

    /// AppKit points and Wayland surface coordinates are both logical units.
    /// Buffer scale controls attached pixel density and must never change an
    /// xdg_toplevel.configure size.
    private func sendConfigure(states: [Windowing.ToplevelState]) {
        guard window != nil else { return }
        let size = Windowing.Size(
            width: max(1, Int(contentView.bounds.width.rounded())),
            height: max(1, Int(contentView.bounds.height.rounded())))
        guard size != lastConfiguredSize || states != lastConfiguredStates else { return }

        pendingConfigure = PendingConfigure(size: size, states: states)
        configureDisplayLink?.isPaused = false
    }

    @objc private func configureDisplayLinkFired(_ displayLink: CADisplayLink) {
        // Deliver the newest resize before waking a frame-throttled client, so
        // the draw started by this tick targets the newest logical size.
        flushConfigure()

        // Frame callbacks and FIFO latching are paced by the display clock.
        // Source buffers were already released by their Metal completion.
        flushPresentations()
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
        guard pending.size != lastConfiguredSize || pending.states != lastConfiguredStates else {
            return
        }
        lastConfiguredSize = pending.size
        lastConfiguredStates = pending.states
        configureSerial &+= 1
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
        let point = surfacePoint(from: point)
        bridge?.send(.pointerEntered(window: windowID, x: point.x, y: point.y))
    }

    func pointerMoved(to point: CGPoint) {
        let point = surfacePoint(from: point)
        bridge?.send(.pointerMoved(window: windowID, x: point.x, y: point.y))
    }

    private func surfacePoint(from windowPoint: CGPoint) -> CGPoint {
        SurfaceCoordinateSpace(
            windowGeometry, contentSize: contentView.bounds.size
        ).surfacePoint(fromContent: windowPoint)
    }

    func pointerLeft() {
        bridge?.send(.pointerLeft(window: windowID))
    }

    func pointerButton(_ button: Windowing.PointerButton, pressed: Bool) {
        // A click anywhere but inside a menu closes it, which is what makes a
        // grab feel like a grab.
        if pressed, !isPopup { bridge?.dismissPopups(ownedBy: windowID) }
        bridge?.send(.pointerButton(window: windowID, button: button, pressed: pressed))
    }

    func pointerScroll(dx: Double, dy: Double, precise: Bool) {
        bridge?.send(.pointerScroll(window: windowID, dx: dx, dy: dy, isPrecise: precise))
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

}

// MARK: - NSWindowDelegate

extension NativeWindow: NSWindowDelegate {
    /// The size is the host's decision; the client redraws to match. Nothing is
    /// stretched in between, so a resize never shows a rubber-banded old frame.
    func windowDidResize(_ notification: Notification) {
        guard !isPopup else { return }
        sendConfigure(states: activeStates())
    }

    /// The drag is over; the client should land on the exact size immediately.
    func windowDidEndLiveResize(_ notification: Notification) {
        sendConfigure(states: activeStates())
        // The final non-resizing state and exact size should not wait for the
        // next turn after AppKit leaves its tracking loop.
        flushConfigure()
    }

    /// Dragging a window to another display changes its backing scale, which in
    /// Wayland terms is a different output scale for that surface.
    func windowDidChangeBackingProperties(_ notification: Notification) {
        guard let window else { return }
        bridge?.send(.scaleChanged(window: windowID, scale: Int(window.backingScaleFactor)))
        sendConfigure(states: activeStates())
    }

    func windowDidBecomeKey(_ notification: Notification) {
        Self.note("window \(windowID) became key")
        guard !isPopup else { return }
        bridge?.send(.keyboardFocus(window: windowID))
        sendConfigure(states: activeStates())
    }

    func windowDidResignKey(_ notification: Notification) {
        Self.note("window \(windowID) resigned key")
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

/// `CAMetalLayer.nextDrawable()` may wait tens of milliseconds for
/// WindowServer. Keeping that wait off AppKit's main thread is essential for
/// mouse motion, live resize and keyboard delivery. This presenter has one
/// running item and one latest-value pending slot, so backpressure cannot grow
/// into an unbounded queue of stale resize frames.
private final class AsyncMetalScenePresenter: @unchecked Sendable {
    private struct Work: @unchecked Sendable {
        let scene: Windowing.SceneSnapshot
        let layers: [ResolvedSceneLayer]
        let readComplete: @MainActor (Bool) -> Void
        let latched: @MainActor () -> Void
    }

    private let layer: CAMetalLayer
    private let renderer: HostSceneRenderer
    private let queue = DispatchQueue(label: "com.nativepipe.metal-present")
    private let lock = NSLock()
    private var pending: Work?
    private var running = false

    init(layer: CAMetalLayer, renderer: HostSceneRenderer) {
        self.layer = layer
        self.renderer = renderer
    }

    func enqueue(
        scene: Windowing.SceneSnapshot, layers: [ResolvedSceneLayer],
        readComplete: @escaping @MainActor (Bool) -> Void,
        latched: @escaping @MainActor () -> Void
    ) {
        let work = Work(
            scene: scene, layers: layers,
            readComplete: readComplete, latched: latched)
        lock.lock()
        let superseded = pending
        pending = work
        let shouldStart = !running
        if shouldStart { running = true }
        lock.unlock()

        if let superseded {
            finish(superseded, success: false)
            latch(superseded)
        }
        if shouldStart {
            queue.async { [weak self] in self?.drain() }
        }
    }

    func cancelPending() {
        lock.lock()
        let cancelled = pending
        pending = nil
        lock.unlock()
        if let cancelled {
            finish(cancelled, success: false)
            latch(cancelled)
        }
    }

    private func takeNext() -> Work? {
        lock.lock()
        defer { lock.unlock() }
        guard let work = pending else {
            running = false
            return nil
        }
        pending = nil
        return work
    }

    private func drain() {
        while let work = takeNext() {
            autoreleasepool {
                guard let drawable = layer.nextDrawable() else {
                    finish(work, success: false)
                    latch(work)
                    return
                }
                do {
                    try renderer.encode(
                        scene: work.scene, layers: work.layers,
                        drawable: drawable
                    ) { command in
                        if command.status != .completed, let error = command.error {
                            FileHandle.standardError.write(
                                Data("[nsw] Metal scene failed: \(error)\n".utf8))
                        }
                        self.finish(work, success: command.status == .completed)
                    }
                    // The scene has been accepted into an ordered drawable and
                    // cannot be overtaken. This is the FIFO latching point;
                    // source-buffer release remains tied to completion above.
                    latch(work)
                } catch {
                    FileHandle.standardError.write(
                        Data("[nsw] could not encode Metal scene: \(error)\n".utf8))
                    finish(work, success: false)
                    latch(work)
                }
            }
        }
    }

    private func finish(_ work: Work, success: Bool) {
        DispatchQueue.main.async { work.readComplete(success) }
    }

    private func latch(_ work: Work) {
        DispatchQueue.main.async { work.latched() }
    }
}

// MARK: - Content view

/// Hosts either the direct CAMetalDrawable path or the remote decoded surface.
private final class SurfaceView: NSView {
    /// Set once the window exists; events before that have nowhere to go.
    weak var input: NativeWindow?

    private var trackingArea: NSTrackingArea?
    private var lastModifiers: NSEvent.ModifierFlags = []

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
    /// Keycodes whose press was actually forwarded. A release for a press the
    /// IME swallowed would leave the guest's xkb state holding a phantom key.
    private var forwardedPresses: Set<UInt16> = []
    /// The legacy CPU/remote surface currently on screen, held for as long as
    /// it is shown. GPU windows present through `metalLayer` instead.
    ///
    /// `ResourceTable` drops its reference the moment the guest unrefs the
    /// resource, which during a resize is while this layer is still displaying
    /// it. Relying on CoreAnimation to have taken a reference is not something
    /// to guess at when the failure mode is a window that blanks at random.
    /// Held so a CPU IOSurface outlives ResourceTable unref during resize.
    private var displayed: IOSurfaceRef?
    private var displayedGeometry: Windowing.Rect?
    /// Maps the committed xdg window geometry to the AppKit content bounds.
    /// During live resize the client may be one configure behind; scaling this
    /// container keeps root pixels, subsurfaces and input in one transform.
    private let sceneLayer = CALayer()
    /// Pixel contents of the root wl_surface. The NSView backing layer is only
    /// the xdg window-geometry clip and never carries surface pixels itself.
    let surfaceLayer = CALayer()
    /// Streaming GPU content uses Core Animation's drawable pool instead of
    /// mutating an IOSurface stored in `CALayer.contents`.
    let metalLayer = CAMetalLayer()

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
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
        metalLayer.isOpaque = false
        metalLayer.anchorPoint = .zero
        metalLayer.isGeometryFlipped = true
        metalLayer.isHidden = true
        wantsLayer = true
        layerContentsRedrawPolicy = .never
    }

    override func makeBackingLayer() -> CALayer {
        let layer = CALayer()
        layer.isOpaque = false
        layer.anchorPoint = .zero
        layer.isGeometryFlipped = true
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
        input?.pointerMoved(to: location(of: event))
    }

    override func rightMouseDragged(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
    }

    override func otherMouseDragged(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
    }

    override func mouseDown(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
        input?.pointerButton(.left, pressed: true)
    }

    override func mouseUp(with event: NSEvent) {
        input?.pointerButton(.left, pressed: false)
    }

    override func rightMouseDown(with event: NSEvent) {
        input?.pointerMoved(to: location(of: event))
        input?.pointerButton(.right, pressed: true)
    }

    override func rightMouseUp(with event: NSEvent) {
        input?.pointerButton(.right, pressed: false)
    }

    override func otherMouseDown(with event: NSEvent) {
        input?.pointerButton(.middle, pressed: true)
    }

    override func otherMouseUp(with event: NSEvent) {
        input?.pointerButton(.middle, pressed: false)
    }

    override func scrollWheel(with event: NSEvent) {
        // macOS scrolls in points and inverts by default; Wayland's axis is a
        // downward-positive distance, so both are undone here rather than in the
        // guest, which cannot know about "natural" scrolling.
        let sign: Double = event.isDirectionInvertedFromDevice ? -1 : 1
        input?.pointerScroll(
            dx: -Double(event.scrollingDeltaX) * sign,
            dy: -Double(event.scrollingDeltaY) * sign,
            precise: event.hasPreciseScrollingDeltas)
    }

    // MARK: Keyboard

    override func keyDown(with event: NSEvent) {
        // Not calling super: NSResponder's default is to beep at anything it does
        // not recognise, and the client is the one deciding what a key means.
        guard !event.isARepeat else { return }
        if textInputEnabled {
            // The input method gets first refusal. What it takes comes back as
            // text; what it declines — Return, Escape, the arrow keys, anything
            // it routes through doCommandBySelector — is an editing command and
            // still belongs on the raw key path.
            unconsumedKeyEvent = event
            _ = inputContext?.handleEvent(event)
            guard let survived = unconsumedKeyEvent else { return }
            unconsumedKeyEvent = nil
            forwardPress(survived)
            return
        }
        forwardPress(event)
    }

    private func forwardPress(_ event: NSEvent) {
        forwardedPresses.insert(event.keyCode)
        input?.key(event.keyCode, pressed: true, flags: event.modifierFlags)
    }

    override func keyUp(with event: NSEvent) {
        guard forwardedPresses.remove(event.keyCode) != nil else { return }
        input?.key(event.keyCode, pressed: false, flags: event.modifierFlags)
    }

    /// Modifiers arrive as a flags snapshot rather than as key events, so the
    /// press and release edges have to be recovered by comparing with the last.
    override func flagsChanged(with event: NSEvent) {
        defer { lastModifiers = event.modifierFlags }
        guard let flag = KeyTranslation.modifierKeyCode(for: event.keyCode) else { return }
        let nowDown = event.modifierFlags.contains(flag)
        let wasDown = lastModifiers.contains(flag)
        guard nowDown != wasDown else { return }
        input?.key(event.keyCode, pressed: nowDown, flags: event.modifierFlags)
    }

    func configureMetalLayer(
        device: MTLDevice, scene: Windowing.SceneSnapshot,
        geometry: Windowing.Rect
    ) -> Bool {
        displayed = nil
        displayedGeometry = geometry
        guard layer != nil, scene.width > 0, scene.height > 0 else { return false }

        let logical = CGRect(
            x: 0, y: 0,
            width: geometry.width, height: geometry.height)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let coordinates = SurfaceCoordinateSpace(geometry, contentSize: bounds.size)
        sceneLayer.bounds = coordinates.sceneBounds
        sceneLayer.position = .zero
        sceneLayer.setAffineTransform(CGAffineTransform(
            scaleX: coordinates.sceneScale.width,
            y: coordinates.sceneScale.height))
        surfaceLayer.isHidden = true
        metalLayer.device = device
        metalLayer.bounds = logical
        metalLayer.frame.origin = .zero
        metalLayer.contentsScale = CGFloat(scene.scale)
        metalLayer.drawableSize = CGSize(width: scene.width, height: scene.height)
        metalLayer.isHidden = false
        CATransaction.commit()
        return true
    }

    /// Remote decoders may allocate surfaces larger than the active frame.
    /// Normalize against the allocation so unused capacity is never stretched.
    func displayCPU(
        _ surface: IOSurfaceRef, frame: Windowing.Frame,
        geometry: Windowing.Rect
    ) {
        displayed = surface
        displayedGeometry = geometry
        guard layer != nil else { return }

        let logical = CGRect(origin: .zero, size: frame.appKitPointSize)
        let contentsRect = frame.contentsRect(
            for: logical,
            allocationSize: CGSize(
                width: IOSurfaceGetWidth(surface),
                height: IOSurfaceGetHeight(surface)))

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let coordinates = SurfaceCoordinateSpace(geometry, contentSize: bounds.size)
        sceneLayer.bounds = coordinates.sceneBounds
        sceneLayer.position = .zero
        sceneLayer.setAffineTransform(CGAffineTransform(
            scaleX: coordinates.sceneScale.width,
            y: coordinates.sceneScale.height))
        surfaceLayer.bounds = logical
        surfaceLayer.frame.origin = .zero
        surfaceLayer.contentsScale = frame.pixelDensity(for: logical)
        surfaceLayer.contentsRect = contentsRect
        surfaceLayer.contents = surface
        surfaceLayer.isHidden = false
        metalLayer.isHidden = true
        CATransaction.commit()
    }

    func clearDisplayedSurface() {
        displayed = nil
        displayedGeometry = nil
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
        message += "surfaceFrame=\(surfaceLayer.frame) surfacePresented=\(surfacePresentation.frame) "
        message += "metalFrame=\(metalLayer.frame) metalPresented=\(metalPresentation.frame) "
        message += "drawable=\(metalLayer.drawableSize)\n"
        FileHandle.standardError.write(Data(message.utf8))
    }

    /// Stretch the last frame across a live resize. The next attach replaces
    /// the drawable; this only covers the gap before it arrives.
    private func layoutDisplayedSurface() {
        guard let geometry = displayedGeometry,
              geometry.width > 0, geometry.height > 0
        else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let coordinates = SurfaceCoordinateSpace(geometry, contentSize: bounds.size)
        sceneLayer.bounds = coordinates.sceneBounds
        sceneLayer.position = .zero
        sceneLayer.setAffineTransform(CGAffineTransform(
            scaleX: coordinates.sceneScale.width,
            y: coordinates.sceneScale.height))
        CATransaction.commit()
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
extension SurfaceView: NSTextInputClient {
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
