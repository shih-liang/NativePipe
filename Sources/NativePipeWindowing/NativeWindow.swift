import AppKit
import IOSurface
import Metal
import NativePipeProtocol
import QuartzCore

/// One `xdg_toplevel`, one `NSWindow`.
///
/// This class is a translator and nothing else. There is no scene graph, no
/// stacking policy, no shadow, no decoration drawing and no compositing pass —
/// macOS already does all of that, and duplicating any of it would mean doing
/// the work twice and then fighting about which answer wins.
///
/// The compositor (guest NativePipe) classifies the buffer at `attach`
/// and names a virtio-gpu resource. This window has one layer:
/// `CAMetalLayer`. CPU and GPU frames both land there.
///
///   * `wl_shm` → compositor copied into an IOSurface → MTLTexture
///   * GPU buffer → Venus resource already an MTLTexture on this host
///
/// A Vulkan swapchain is just a sequence of GPU attaches. Present does
/// not need a second bind.
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
    /// Absence of xdg-decoration means the client owns its decorations.
    /// xdg-decoration is optional, and a client that never binds it has not
    /// asked for anything — it is not claiming it will draw its own chrome. The
    /// compositor decides in that case, and the only answer that leaves a usable
    /// window is server-side. Defaulting to false put such a window in the gap
    /// between the two: AppKit hid its title bar for a client that was never
    /// going to draw one.
    private var serverDecorated = true
    private var lastConfiguredSize: Windowing.Size?
    private var lastConfiguredStates: [Windowing.ToplevelState] = []
    private var configureSerial: UInt32 = 0

    /// The scale the *client* renders at, from `wl_surface.set_buffer_scale`.
    ///
    /// Not `NSWindow.backingScaleFactor`. Converting the window's point size to
    /// surface pixels with the screen's scale tells a scale-1 client that its
    /// 480pt window is 960px wide; it redraws at 960x640, the window grows to
    /// 960pt, and the next configure says 1920. Buffer geometry is the client's
    /// declaration, so the client's scale is the only correct divisor.
    private var bufferScale = 1
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

    init(windowID: UInt32, surfaceID: UInt32, bridge: WindowBridge, popup: Popup? = nil) {
        self.windowID = windowID
        self.surfaceID = surfaceID
        self.bridge = bridge
        self.popup = popup
        super.init()
    }

    var isPopup: Bool { popup != nil }

    /// Where subsurface layers are hung. They are part of this window's
    /// contents, not windows of their own, so CoreAnimation composites them and
    /// NativePipe does not.
    var contentLayer: CALayer? { window != nil ? contentView.layer : nil }

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
        guard let window else { return }
        if let minimum {
            window.contentMinSize = NSSize(width: minimum.width, height: minimum.height)
        }
        if let maximum {
            window.contentMaxSize = NSSize(width: maximum.width, height: maximum.height)
        }
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
        contentView.displayCPU(
            surface, frame: frame, geometry: windowGeometry,
            scale: CGFloat(bufferScale))
        if Self.frameTrace {
            Self.note("present cpu window=\(windowID) frame=\(frame.width)x\(frame.height)@\(frame.scale)")
        }
    }

    /// Venus image already resident in MoltenVK. Present it the way a
    /// native macOS Vulkan window does: onto this window's CAMetalLayer.
    func presentGPU(
        frame: Windowing.Frame, pointer: UnsafeMutableRawPointer, byteCount: Int
    ) {
        prepareWindow(for: frame)
        contentView.displayGPU(
            pointer: pointer, byteCount: byteCount, frame: frame,
            geometry: windowGeometry, scale: CGFloat(bufferScale))
        if Self.frameTrace {
            Self.note("present gpu window=\(windowID) frame=\(frame.width)x\(frame.height)@\(frame.scale)")
        }
    }

    func presentGPU(frame: Windowing.Frame, metalTexture: AnyObject) {
        prepareWindow(for: frame)
        contentView.displayGPU(
            metalTexture: metalTexture, frame: frame,
            geometry: windowGeometry, scale: CGFloat(bufferScale))
        if Self.frameTrace {
            Self.note("present gpu-mtl window=\(windowID) frame=\(frame.width)x\(frame.height)@\(frame.scale)")
        }
    }

    private func prepareWindow(for frame: Windowing.Frame) {
        bufferScale = max(frame.scale, 1)
        windowGeometry = effectiveGeometry(for: frame)
        let pointSize = NSSize(
            width: CGFloat(windowGeometry.width),
            height: CGFloat(windowGeometry.height))
        if window == nil {
            makeWindow(contentSize: pointSize)
        }
    }

    private func effectiveGeometry(for frame: Windowing.Frame) -> Windowing.Rect {
        if let geometry = frame.windowGeometry,
           geometry.width > 0, geometry.height > 0 {
            return geometry
        }
        return Windowing.Rect(
            x: 0, y: 0,
            width: frame.width / max(frame.scale, 1),
            height: frame.height / max(frame.scale, 1))
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
        window.contentView = contentView
        contentView.input = self
        window.delegate = self
        window.tabbingMode = .disallowed
        // Tracking areas request motion in their bounds, and this also enables
        // the ordinary responder-chain path on AppKit versions that consult the
        // window flag first. It is false by default.
        window.acceptsMouseMovedEvents = true

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
        self.window = window
        let displayLink = contentView.displayLink(
            target: self, selector: #selector(configureDisplayLinkFired(_:)))
        displayLink.isPaused = true
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
        configureDisplayLink?.invalidate()
        configureDisplayLink = nil
        window?.orderOut(nil)
        window?.delegate = nil
        window = nil
    }

    // MARK: - Geometry

    /// Translates the window's current size into a configure for the client.
    private func sendConfigure(states: [Windowing.ToplevelState]) {
        guard window != nil else { return }
        let size = Windowing.Size(
            width: max(1, Int(contentView.bounds.width)) * bufferScale,
            height: max(1, Int(contentView.bounds.height)) * bufferScale)
        guard size != lastConfiguredSize || states != lastConfiguredStates else { return }

        pendingConfigure = PendingConfigure(size: size, states: states)
        configureDisplayLink?.isPaused = false
    }

    @objc private func configureDisplayLinkFired(_ displayLink: CADisplayLink) {
        flushConfigure()
    }

    private func flushConfigure() {
        guard let pending = pendingConfigure else {
            configureDisplayLink?.isPaused = true
            return
        }
        pendingConfigure = nil
        configureDisplayLink?.isPaused = true
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
        CGPoint(
            x: windowPoint.x + CGFloat(windowGeometry.x),
            y: windowPoint.y + CGFloat(windowGeometry.y))
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

// MARK: - Content view

/// The window's only layer is a `CAMetalLayer`. Attach already decided
/// whether the named resource is an IOSurface or a Venus mapping.
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
    /// The surface currently on screen, held for as long as it is shown.
    ///
    /// `ResourceTable` drops its reference the moment the guest unrefs the
    /// resource, which during a resize is while this layer is still displaying
    /// it. Relying on CoreAnimation to have taken a reference is not something
    /// to guess at when the failure mode is a window that blanks at random.
    /// Held so a CPU IOSurface outlives ResourceTable unref during resize.
    private var displayed: IOSurfaceRef?
    private var metal: MetalPresenter?
    private var displayedGeometry: Windowing.Rect?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
    }

    override func makeBackingLayer() -> CALayer {
        let layer = CAMetalLayer()
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = false
        layer.isOpaque = false
        layer.contentsGravity = .resize
        layer.anchorPoint = .zero
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

    /// The surface is exactly the size of the frame in it, so the whole thing is
    /// shown. Padding the allocation and displaying a sub-rectangle was tried and
    /// reverted: it needs contentsRect, whose origin corner behaves differently
    /// for an IOSurface than the obvious probe suggests, and the win disappears
    /// once configures are coalesced to the latest size.
    func displayCPU(
        _ surface: IOSurfaceRef, frame: Windowing.Frame,
        geometry: Windowing.Rect, scale: CGFloat
    ) {
        displayed = surface
        displayedGeometry = geometry
        guard let presenter = presenter() else { return }
        presenter.present(
            presenter.texture(from: surface, frame: frame),
            geometry: geometry, scale: scale)
    }

    func displayGPU(
        pointer: UnsafeMutableRawPointer, byteCount: Int,
        frame: Windowing.Frame, geometry: Windowing.Rect, scale: CGFloat
    ) {
        displayed = nil
        displayedGeometry = geometry
        guard let presenter = presenter() else { return }
        presenter.present(
            presenter.texture(pointer: pointer, byteCount: byteCount, frame: frame),
            geometry: geometry, scale: scale)
    }

    func displayGPU(
        metalTexture: AnyObject, frame: Windowing.Frame,
        geometry: Windowing.Rect, scale: CGFloat
    ) {
        displayed = nil
        displayedGeometry = geometry
        guard let presenter = presenter(),
              let texture = metalTexture as? MTLTexture else { return }
        presenter.present(texture, geometry: geometry, scale: scale)
    }

    private func presenter() -> MetalPresenter? {
        if let metal { return metal }
        guard let metalLayer = layer as? CAMetalLayer else { return nil }
        metal = MetalPresenter(layer: metalLayer)
        return metal
    }

    /// Stretch the last frame across a live resize. The next attach replaces
    /// the drawable; this only covers the gap before it arrives.
    private func layoutDisplayedSurface() {
        guard let geometry = displayedGeometry,
              geometry.width > 0, geometry.height > 0
        else { return }
        let scale = layer?.contentsScale ?? 1
        (layer as? CAMetalLayer)?.drawableSize = CGSize(
            width: CGFloat(geometry.width) * scale,
            height: CGFloat(geometry.height) * scale)
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

/// Presents an already-host texture onto the window's CAMetalLayer.
///
/// The layer is the view. CPU and GPU only differ in how the source
/// texture is obtained; attach already bound the resource.
private final class MetalPresenter {
    private let layer: CAMetalLayer
    private let device: MTLDevice
    private let queue: MTLCommandQueue

    init?(layer: CAMetalLayer) {
        guard let device = layer.device ?? MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else { return nil }
        self.layer = layer
        self.device = device
        self.queue = queue
        layer.device = device
    }

    func present(
        _ source: MTLTexture?,
        geometry: Windowing.Rect, scale: CGFloat
    ) {
        guard let source else { return }
        let crop = pixelCrop(geometry: geometry, scale: scale, texture: source)
        layer.contentsScale = scale
        layer.drawableSize = CGSize(width: crop.width, height: crop.height)
        guard let drawable = layer.nextDrawable(),
              let command = queue.makeCommandBuffer(),
              let blit = command.makeBlitCommandEncoder() else { return }
        let width = min(crop.width, drawable.texture.width)
        let height = min(crop.height, drawable.texture.height)
        blit.copy(
            from: source, sourceSlice: 0, sourceLevel: 0,
            sourceOrigin: MTLOrigin(x: crop.x, y: crop.y, z: 0),
            sourceSize: MTLSize(width: width, height: height, depth: 1),
            to: drawable.texture, destinationSlice: 0, destinationLevel: 0,
            destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        blit.endEncoding()
        command.present(drawable)
        command.commit()
    }

    func texture(from surface: IOSurfaceRef, frame: Windowing.Frame) -> MTLTexture? {
        let width = max(frame.width, 1)
        let height = max(frame.height, 1)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: frame.format == .rgba8888 ? .rgba8Unorm : .bgra8Unorm,
            width: width, height: height, mipmapped: false)
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderRead]
        return device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
    }

    func texture(
        pointer: UnsafeMutableRawPointer, byteCount: Int, frame: Windowing.Frame
    ) -> MTLTexture? {
        let stride = frame.bytesPerRow
        let needed = stride * frame.height
        guard byteCount >= needed else { return nil }
        let format: MTLPixelFormat = frame.format == .rgba8888 ? .rgba8Unorm : .bgra8Unorm
        let align = device.minimumLinearTextureAlignment(for: format)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: frame.width, height: frame.height, mipmapped: false)
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderRead]

        if align > 0, stride % align == 0,
           let buffer = device.makeBuffer(
            bytesNoCopy: pointer, length: needed,
            options: .storageModeShared, deallocator: nil) {
            return buffer.makeTexture(descriptor: descriptor, offset: 0, bytesPerRow: stride)
        }

        // Stride is not Metal-linear-aligned. One upload into a private
        // texture, then the blit to the drawable. Still host-side only.
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderRead, .shaderWrite]
        guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
        texture.replace(
            region: MTLRegionMake2D(0, 0, frame.width, frame.height),
            mipmapLevel: 0,
            withBytes: pointer,
            bytesPerRow: stride)
        return texture
    }

    /// xdg window geometry in buffer pixels. CSD shadows sit outside it.
    private func pixelCrop(
        geometry: Windowing.Rect, scale: CGFloat, texture: MTLTexture
    ) -> (x: Int, y: Int, width: Int, height: Int) {
        let s = max(Int(scale), 1)
        var x = geometry.x * s
        var y = geometry.y * s
        var width = geometry.width * s
        var height = geometry.height * s
        if width <= 0 || height <= 0 {
            return (0, 0, texture.width, texture.height)
        }
        x = min(max(x, 0), texture.width)
        y = min(max(y, 0), texture.height)
        width = min(width, texture.width - x)
        height = min(height, texture.height - y)
        return (x, y, max(width, 1), max(height, 1))
    }
}
