import NativePipeStrings
import AppKit
import CoreGraphics
import CoreImage
import ImageIO
import IOSurface
@preconcurrency import Metal
import NativePipeProtocol
import UniformTypeIdentifiers
import QuartzCore

public enum WindowPresentationPauseError: LocalizedError {
    case timedOut
    case connectionChanged
    case busy

    public var errorDescription: String? {
        switch self {
        case .timedOut: return NPText("The display did not finish preparing for the virtual machine state change.")
        case .connectionChanged: return NPText("The display connection changed during the virtual machine state change.")
        case .busy: return NPText("Another display state change is already in progress.")
        }
    }
}

public enum FrameTextureStatus: Sendable {
    case ready
    case unpublished
    case unavailable
}

public struct WindowIntegrationPreferences: Sendable, Equatable {
    /// Nil follows AppKit's already-applied system direction.
    public var naturalScrolling: Bool?
    public var shortcuts: KeyboardShortcutPreferences
    public var clipboardHostToGuest: Bool
    public var clipboardGuestToHost: Bool
    public var keyboardLayout: String
    public var keyRepeatRate: Int
    public var keyRepeatDelay: Int

    public init(
        naturalScrolling: Bool? = nil,
        shortcuts: KeyboardShortcutPreferences = .init(),
        clipboardHostToGuest: Bool = true,
        clipboardGuestToHost: Bool = true,
        keyboardLayout: String = "us",
        keyRepeatRate: Int = 25,
        keyRepeatDelay: Int = 600
    ) {
        self.naturalScrolling = naturalScrolling
        self.shortcuts = shortcuts
        self.clipboardHostToGuest = clipboardHostToGuest
        self.clipboardGuestToHost = clipboardGuestToHost
        self.keyboardLayout = keyboardLayout
        self.keyRepeatRate = keyRepeatRate
        self.keyRepeatDelay = keyRepeatDelay
    }
}

/// Result of resolving one exact scene layer. Only `unpublished` is retryable:
/// the CREATE_BLOB command has not crossed the virtio queue yet.
public struct FrameTextureResolution: @unchecked Sendable {
    public let status: FrameTextureStatus
    public let texture: AnyObject?
    public let surface: IOSurfaceRef?
    /// Keeps pooled pixels occupied independently of the provider's cache.
    public let owner: AnyObject?

    public init(status: FrameTextureStatus, texture: AnyObject? = nil,
                surface: IOSurfaceRef? = nil, owner: AnyObject? = nil) {
        self.status = status
        self.texture = texture
        self.surface = surface
        self.owner = owner
    }
}

public struct DockWindow: Sendable, Identifiable, Equatable {
    public let id: UInt32
    public let title: String
    public let applicationID: String?
    public let applicationName: String?
    public let isMiniaturized: Bool
    public let isZoomed: Bool
    public let isFullscreen: Bool
    public let width: Double
    public let height: Double
    public let isVisible: Bool
    public let isKey: Bool
    public let canForceQuit: Bool
    /// Outer window rectangle in AppKit's global, bottom-left screen points.
    public let frame: CGRect?

    public init(
        id: UInt32, title: String, applicationID: String?,
        isMiniaturized: Bool = false, isZoomed: Bool = false,
        isFullscreen: Bool = false, width: Double = 0, height: Double = 0,
        isVisible: Bool = false, isKey: Bool = false,
        canForceQuit: Bool = false, frame: CGRect? = nil, applicationName: String? = nil
    ) {
        self.id = id
        self.title = title
        self.applicationID = applicationID
        self.applicationName = applicationName
        self.isMiniaturized = isMiniaturized
        self.isZoomed = isZoomed
        self.isFullscreen = isFullscreen
        self.width = width
        self.height = height
        self.isVisible = isVisible
        self.isKey = isKey
        self.canForceQuit = canForceQuit
        self.frame = frame
    }
}

public struct DockWindowCapture: Sendable {
    public let png: Data
    public let contentSize: CGSize
    public let pixelSize: CGSize

    public init(png: Data, contentSize: CGSize, pixelSize: CGSize) {
        self.png = png
        self.contentSize = contentSize
        self.pixelSize = pixelSize
    }
}

public enum ComputerUseWindowError: LocalizedError {
    case unavailable
    case hidden
    case invalidCoordinates
    case captureFailed

    public var errorDescription: String? {
        switch self {
        case .unavailable: NPText("the guest window no longer exists")
        case .hidden: NPText("the guest window is not visible")
        case .invalidCoordinates: NPText("input coordinates are outside the guest content")
        case .captureFailed: NPText("Could not capture the guest window")
        }
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
    func resolveFrame(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> FrameTextureResolution
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

    public func resolveFrame(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> FrameTextureResolution {
        let texture = metalTexture(forResource: resourceID, width: width,
            height: height, bytesPerRow: bytesPerRow, format: format)
        let surface = surface(forResource: resourceID)
        return FrameTextureResolution(
            status: texture != nil || surface != nil ? .ready :
                (isResourcePublished(resourceID) ? .unavailable : .unpublished),
            texture: texture, surface: surface)
    }

    public func metalTextures(
        for layers: [Windowing.SceneLayer],
        completion: @escaping @MainActor ([FrameTextureResolution]) -> Void
    ) {
        completion(layers.map {
            resolveFrame(
                forResource: $0.resourceID,
                width: $0.width, height: $0.height,
                bytesPerRow: $0.bytesPerRow,
                format: $0.format == .rgba8888 ? 67 : 1)
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

    /// The smallest long edge, in points, of a cursor that was divided down from
    /// physical pixels. A bitmap core cursor of 16 pixels would otherwise
    /// become 8 points, smaller than any macOS pointer.
    static let minimumPhysicalPixelCursorEdge: CGFloat = 16

    /// `pixelScale` above 1 means the client drew the cursor in physical pixels
    /// (an X11 client behind xwayland-satellite, which hides the display scale
    /// from Xwayland). Its size and hotspot are converted to points; without
    /// this a 48-pixel cursor requested by a scaled toolkit showed 48 points tall.
    static func customCursorGeometry(
        frame: Windowing.Frame, hotSpot requestedHotSpot: CGPoint, pixelScale: Int = 1
    ) -> CustomCursorGeometry {
        let logical = frame.appKitPointSize
        var factor: CGFloat = 1
        if pixelScale > 1 {
            factor = 1 / CGFloat(pixelScale)
            let edge = max(logical.width, logical.height) * factor
            if edge > 0, edge < minimumPhysicalPixelCursorEdge {
                factor *= minimumPhysicalPixelCursorEdge / edge
            }
        }
        let width = max(logical.width * factor, 1)
        let height = max(logical.height * factor, 1)
        return CustomCursorGeometry(
            imageSize: CGSize(width: width, height: height),
            sourcePixels: frame.fullViewportBufferPixelRect.integral,
            hotSpot: CGPoint(
                x: min(max(requestedHotSpot.x * factor, 0), max(0, width - 1)),
                y: min(max(requestedHotSpot.y * factor, 0), max(0, height - 1))))
    }

    static func topDownCursorImage(_ image: CIImage, source: CGRect) -> CIImage {
        image.cropped(to: source).transformed(by: CGAffineTransform(
            a: 1, b: 0, c: 0, d: -1,
            tx: 0, ty: source.minY + source.maxY))
    }

    private var dragIconPresenter: AuxiliarySurfacePresenter?
    private var cursorPresenter: AuxiliarySurfacePresenter?
    private var dragIcon: AuxiliarySurfacePresenter {
        if let dragIconPresenter { return dragIconPresenter }
        let presenter = AuxiliarySurfacePresenter(kind: .drag, displayClock: displayClock)
        presenter.onNeedsNewPublication = { [weak self] surface, _ in
            self?.requestAuxiliaryPublication(surface: surface)
        }
        dragIconPresenter = presenter
        return presenter
    }
    private var customCursor: AuxiliarySurfacePresenter {
        if let cursorPresenter { return cursorPresenter }
        let presenter = AuxiliarySurfacePresenter(kind: .cursor, displayClock: displayClock)
        presenter.onNeedsNewPublication = { [weak self] surface, _ in
            self?.requestAuxiliaryPublication(surface: surface)
        }
        cursorPresenter = presenter
        return presenter
    }
    private var dragExportSuppressed = false
    private var pointerPresentationWindow: UInt32?
    private var pointerPresentationPosition = CGPoint.zero
    private var auxiliaryWasVisible = false
    private var auxiliaryRecords: [PresentationJournal.Key: PresentationRecord] = [:]
    private var deferredPresentationRefresh: Set<UInt32> = []
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
        let record: PresentationRecord?

        init(scene: Windowing.SceneSnapshot, record: PresentationRecord?) {
            self.scene = scene
            self.record = record
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
    /// Optional transport feedback: true comes from the drawable's actual
    /// presentation handler; superseded/cancelled scenes report false.
    /// This is separate from Wayland frame/FIFO latch completion.
    public var onScenePresentation: ((UInt32, UInt32, Bool, UInt32) -> Void)?
    /// Actual Wayland feedback is independent of NPRP transport display credit.
    public var reportsPresentationTime = true
    private var deferredSceneFeedback = DeferredSceneFeedback()
    private lazy var presentationJournal = PresentationJournal { [weak self] in
        guard let bridge = self else { return }
        MainRunLoop.perform { [weak bridge] in bridge?.flushPresentationResults() }
    }
    private var presentationSessionID: UInt64?
    private var sentPresentationResults: Set<PresentationJournal.Key> = []
    private var presentationDrawingGated = false
    private var presentationBarrierInFlight = false
    private struct CompletedPauseRollback: Sendable {
        let generation: UInt64
        let sessionID: UInt64?
    }
    private var completedPauseRollback: CompletedPauseRollback?
    private var nextPresentationBarrierToken: UInt32 = 0
    private enum PresentationBarrierPhase { case pause, drain, resume }
    private struct PresentationBarrier {
        let sessionID: UInt64
        let token: UInt32
        let phase: PresentationBarrierPhase
        var reached = false
    }
    private var presentationBarrier: PresentationBarrier?

    func scenePresented(surface: UInt32, presentationID: UInt32, displayed: Bool,
                        presentedTime: Double? = nil) {
        guard onScenePresentation != nil else { return }
        let native = surfaceToWindow[surface].flatMap { windows[$0] }
        let occluded = native?.window != nil && native?.canPresent == false
        if deferredSceneFeedback.shouldSend(
            surface: surface, presentationID: presentationID,
            displayed: displayed, occluded: occluded) {
            onScenePresentation?(surface, presentationID, displayed, displayInterval(for: surface))
        }
    }

    func flushSceneFeedback(surface: UInt32) {
        for id in deferredSceneFeedback.take(surface: surface) {
            onScenePresentation?(surface, id, false, displayInterval(for: surface))
        }
    }

    private func displayInterval(for surface: UInt32) -> UInt32 {
        surfaceToWindow[surface].flatMap { windows[$0]?.displayIntervalNanoseconds } ?? 0
    }

    private func acceptPresentationSession(_ sessionID: UInt64) {
        guard sessionID != 0 else { return }
        if presentationSessionID != sessionID {
            presentationJournal.retainSession(sessionID)
            sentPresentationResults.removeAll()
            presentationSessionID = sessionID
        }
        flushPresentationResults()
    }

    private func flushPresentationResults(replay: Bool = false) {
        guard reportsPresentationTime, output != nil,
              let sessionID = presentationSessionID else { return }
        for result in presentationJournal.results(sessionID: sessionID) {
            guard replay || !sentPresentationResults.contains(result.key) else { continue }
            send(.presentationFeedback(
                sessionID: result.key.sessionID, clockEpoch: result.key.clockEpoch,
                surface: result.key.surface, presentationID: result.key.presentationID,
                hostTimeNanoseconds: result.hostTimeNanoseconds,
                refreshNanoseconds: result.refreshNanoseconds, outputID: result.outputID))
            sentPresentationResults.insert(result.key)
        }
    }

    private func hasDiscardAwaitingAcknowledgement(surface: UInt32) -> Bool {
        guard let sessionID = presentationSessionID else { return false }
        return presentationJournal.results(sessionID: sessionID).contains {
            $0.key.surface == surface && $0.hostTimeNanoseconds == 0
        }
    }

    func flushDeferredPresentationRefresh(surface: UInt32) {
        guard deferredPresentationRefresh.contains(surface),
              !hasDiscardAwaitingAcknowledgement(surface: surface) else { return }
        guard !presentationDrawingGated, !presentationSuspended else { return }
        if surface == cursorSurface {
            guard cursorUsesSoftwarePresentation || cursorPublicationIsQueried else {
                deferredPresentationRefresh.remove(surface)
                return
            }
            guard auxiliaryPointerVisible else { return }
        } else if surface == dragIconSurface {
            guard !dragExportSuppressed else {
                deferredPresentationRefresh.remove(surface)
                return
            }
            guard auxiliaryPointerVisible else { return }
        } else {
            guard let native = surfaceToWindow[surface].flatMap({ windows[$0] }) else {
                deferredPresentationRefresh.remove(surface)
                return
            }
            guard native.canPresent || native.hasPendingFrameCapture else { return }
        }
        send(.captureFrame(surface: surface))
    }
    private var pointerCursor = NSCursor.arrow
    private var cursorSurface: UInt32?
    private var cursorHotSpot = CGPoint.zero
    private var cursorPresentationHotSpot = CGPoint.zero
    private var cursorGeometryFrame: Windowing.Frame?
    private var cursorUsesSoftwarePresentation = false
    private var cursorPublicationIsQueried = false
    private var cursorContext: CIContext?
    private var cursorPixelScale = 1
    private lazy var transparentPointerCursor = NSCursor(
        image: NSImage(size: NSSize(width: 1, height: 1), flipped: false) { _ in true },
        hotSpot: .zero)

    private var windows: [UInt32: NativeWindow] = [:]
    private var forceQuitCapabilities: [UInt32: Bool] = [:]
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
    private var presentationSuspended = false
    private var suspendedVisibleWindows: [UInt32] = []
    private var suspendedKeyWindow: UInt32?
    private var computerPointerWindow: UInt32?
    private var integrationPreferences = WindowIntegrationPreferences()
    private var keyUpMonitor: Any?

    /// Strong on purpose. There is no cycle to break — a frame source refers to
    /// the VM controller weakly, if at all — and a weak reference here silently
    /// drops every frame the moment the caller stops holding the source itself.
    private let frameSource: FrameSource?

    /// Sends a command down to the guest translator. Each host wires it to its
    /// own transport -- vsock for the VM host, SSH stdio for the remote one --
    /// and the demo driver substitutes its own sink.
    public var output: ((Windowing.HostCommand) -> Void)?
    private(set) var connectionGeneration: UInt64 = 0
    /// Fired once when a toplevel has both an app id and a materialized
    /// NSWindow. This is the launcher's end-to-end success signal.
    public var onApplicationWindowMapped: ((String) -> Void)?
    /// A guest application posted or updated a desktop notification. The
    /// embedding app decides whether and how to present it; unset drops it.
    public var onGuestNotification: ((Windowing.GuestNotification) -> Void)?
    /// A guest application withdrew a notification it posted earlier.
    public var onGuestNotificationClosed: ((UInt32, UInt64) -> Void)?
    public var onGuestNotificationBacklogReset: (() -> Void)?
    /// A compositor handshake starts a new notification ID namespace as well
    /// as a new window graph, including a reconnect inside a running VM.
    public var onChannelReady: (() -> Void)?
    /// Mapped toplevel presence, independent of app IDs, occlusion and
    /// minimization. Hosts use this to own their Dock activation policy.
    public var onWindowPresenceChanged: ((Bool) -> Void)? {
        didSet { onWindowPresenceChanged?(hasApplicationWindows) }
    }
    private var reportedWindowPresence = false
    public var hasApplicationWindows: Bool {
        windows.values.contains { !$0.isPopup && $0.window != nil }
    }

    func updateWindowPresence() {
        notifyDockWindowsChanged()
        let present = hasApplicationWindows
        guard present != reportedWindowPresence else { return }
        reportedWindowPresence = present
        onWindowPresenceChanged?(present)
    }
    public var applicationIconProvider: ((String) -> NSImage?)?
    public var applicationNameProvider: ((String) -> String?)?
    public var machineName: String = "" {
        didSet {
            guard machineName != oldValue else { return }
            for window in windows.values { window.refreshHostChrome() }
            notifyDockWindowsChanged()
        }
    }
    static let dockWindowsDidChange = Notification.Name("NativePipeDockWindowsDidChange")
    func notifyDockWindowsChanged() {
        NotificationCenter.default.post(name: Self.dockWindowsDidChange, object: self)
    }

    let clipboard: ClipboardBridge
    public var fileAccess: (any UserFileAccess)? {
        didSet { clipboard.fileAccess = fileAccess }
    }
    public var publishGuestFiles: GuestFilePublisher? {
        didSet { clipboard.publishGuestFiles = publishGuestFiles }
    }
    public var onGuestFileSharingRevoked: (() -> Void)?
    public var onClipboardFileSharingRevoked: (() -> Void)? {
        didSet { clipboard.onFileSharingRevoked = onClipboardFileSharingRevoked }
    }
    lazy var fileDrag = FileDragBridge(bridge: self)

    func containsGuestWindow(at point: NSPoint) -> Bool {
        windows.values.contains { $0.window.map { $0.isVisible && $0.frame.contains(point) } ?? false }
    }
    func hideDragIcon() {
        dragExportSuppressed = true
        dragIconPresenter?.setVisible(false)
        if let surface = dragIconSurface { retireAuxiliaryFrames(surface: surface) }
    }
    func reportFileTransferError(_ error: Error) { NSApp.presentError(error) }

    public init(frameSource: FrameSource?) {
        self.frameSource = frameSource
        clipboard = ClipboardBridge()
		super.init()
        clipboard.output = { [weak self] command in self?.send(command) }
        clipboard.onError = { [weak self] error in self?.reportFileTransferError(error) }
        clipboard.start()
        // Keep AppKit's normal shortcut/menu dispatch. Only rescue releases
        // for presses sent by our own guest views; never monitor other apps.
        keyUpMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyUp) { [weak self] event in
            let handled = MainActor.assumeIsolated { self?.handleKeyUp(event) == true }
            return handled ? nil : event
        }
		NotificationCenter.default.addObserver(
			self, selector: #selector(screenParametersChanged(_:)),
			name: NSApplication.didChangeScreenParametersNotification, object: nil)
        NotificationCenter.default.addObserver(
            self, selector: #selector(applicationDidResignActive(_:)),
            name: NSApplication.didResignActiveNotification, object: nil)
    }

	deinit {
		if let keyUpMonitor { NSEvent.removeMonitor(keyUpMonitor) }
		NotificationCenter.default.removeObserver(self)
	}

    func handleKeyUp(_ event: NSEvent) -> Bool {
        guard event.type == .keyUp,
              let window = event.window ?? NSApp.keyWindow,
              let native = windows.values.first(where: { $0.window === window }) else { return false }
        return native.handleKeyUp(event)
    }

    var acceptsKeyboardInput: Bool { !presentationSuspended && !presentationDrawingGated }

    @objc private func applicationDidResignActive(_ notification: Notification) {
        pointerPresentationWindow = nil
        updateAuxiliaryVisibility()
        refreshPointerCursor()
        for native in windows.values {
            native.endScrollGesture()
            native.releasePressedKeys()
        }
    }

    public var windowCount: Int { windows.count }

    public func setIntegrationPreferences(_ value: WindowIntegrationPreferences) {
        guard value != integrationPreferences else { return }
        let keyboardChanged = value.keyboardLayout != integrationPreferences.keyboardLayout
            || value.keyRepeatRate != integrationPreferences.keyRepeatRate
            || value.keyRepeatDelay != integrationPreferences.keyRepeatDelay
        if value.keyboardLayout != integrationPreferences.keyboardLayout {
            // A new XKB layout cannot reinterpret keys still held in the old one.
            for native in windows.values { native.releasePressedKeys() }
        }
        integrationPreferences = value
        clipboard.setPolicy(
            hostToGuest: value.clipboardHostToGuest,
            guestToHost: value.clipboardGuestToHost)
        if keyboardChanged { sendInputPreferences() }
    }

    func scrollDeltas(for event: NSEvent) -> (dx: Double, dy: Double) {
        let multiplier: Double
        if let natural = integrationPreferences.naturalScrolling {
            multiplier = natural == event.isDirectionInvertedFromDevice ? 1 : -1
        } else {
            multiplier = 1
        }
        return (
            -Double(event.scrollingDeltaX) * multiplier,
            -Double(event.scrollingDeltaY) * multiplier)
    }

    func scrollDirectionInverted(for event: NSEvent) -> Bool {
        integrationPreferences.naturalScrolling ?? event.isDirectionInvertedFromDevice
    }

    var shortcutPreferences: KeyboardShortcutPreferences {
        integrationPreferences.shortcuts
    }

    private func sendInputPreferences() {
        send(.inputPreferences(
            layout: integrationPreferences.keyboardLayout,
            repeatRate: integrationPreferences.keyRepeatRate,
            repeatDelay: integrationPreferences.keyRepeatDelay))
    }

    /// Freeze host presentation without changing Wayland object lifetime.
    /// `orderOut` is deliberately not `close`: the guest remains authoritative
    /// and sees the same xdg_toplevels after VZ resumes.
    public func setSuspended(_ suspended: Bool, hideWindows: Bool = false) {
        if suspended { completedPauseRollback = nil }
        guard presentationSuspended != suspended else { return }
        if suspended {
            for native in windows.values {
                native.endScrollGesture()
                native.releasePressedKeys()
            }
        }
        presentationSuspended = suspended
        updateAuxiliaryVisibility()
        if suspended {
            for native in windows.values { unregisterDisplayClock(native) }
            guard hideWindows else { return }
            suspendedVisibleWindows = windows.compactMap { id, native in
                native.window?.isVisible == true ? id : nil
            }
            suspendedKeyWindow = windows.first { $0.value.window?.isKeyWindow == true }?.key
            for id in suspendedVisibleWindows { windows[id]?.window?.orderOut(nil) }
            return
        }

        for native in windows.values {
            native.invalidateSceneHistory()
            registerDisplayClock(native, screen: native.window?.screen)
        }
        for id in suspendedVisibleWindows { windows[id]?.window?.orderFront(nil) }
        if NSApp.isActive, let id = suspendedKeyWindow {
            windows[id]?.window?.makeKey()
        }
        suspendedVisibleWindows.removeAll(keepingCapacity: true)
        suspendedKeyWindow = nil
        retryPendingFrames()
        refreshAuxiliaryFrames()
    }

    /// Close submission first, fence the display stream and actual drawable
    /// callbacks, then ask the guest to consume all terminal feedback. A
    /// timeout leaves submitted records intact and attempts a calibrated resume.
    public func prepareForVirtualMachinePause() async throws {
        guard !presentationBarrierInFlight else { throw WindowPresentationPauseError.busy }
        presentationBarrierInFlight = true
        defer { presentationBarrierInFlight = false }
        completedPauseRollback = nil
        let generation = connectionGeneration
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        setPresentationDrawingGated(true)
        for surface in Set(pendingScenes.keys).union(resolvingScenes.keys) {
            cancelSceneWork(for: surface)
        }
        for surface in Set(pendingSurfaceFrames.keys).union(pendingFrames.keys) {
            retireAuxiliaryFrames(surface: surface)
        }
        do {
            if let sessionID = presentationSessionID {
                let token = newPresentationBarrierToken()
                try await requestPresentationBarrier(.pause, sessionID: sessionID,
                    token: token, generation: generation, deadline: deadline,
                    command: .presentationPause(sessionID: sessionID, token: token))
                try await fencePresentationSubmission(generation: generation, deadline: deadline)
                try await waitForPresentationCondition(generation: generation, deadline: deadline) {
                    !self.presentationJournal.hasSubmitted(sessionID: sessionID)
                }
                for native in windows.values { native.flushPresentationLatchesForPause() }
                // Latches/configures use the control lane, while actual
                // feedback and drain use a different writer. A second control
                // receipt proves those late latches crossed the guest before
                // the feedback fence can authorize VZ to stop executing it.
                let controlFenceToken = newPresentationBarrierToken()
                try await requestPresentationBarrier(.pause, sessionID: sessionID,
                    token: controlFenceToken, generation: generation, deadline: deadline,
                    command: .presentationPause(sessionID: sessionID, token: controlFenceToken))
                flushPresentationResults(replay: true)
                try await requestPresentationBarrier(.drain, sessionID: sessionID,
                    token: controlFenceToken, generation: generation, deadline: deadline,
                    command: .presentationDrain(sessionID: sessionID, token: controlFenceToken))
            } else {
                // No live compositor requires no protocol handshake, but a
                // detached old drawable can still own an unrecorded result.
                try await fencePresentationSubmission(generation: generation, deadline: deadline)
                try await waitForPresentationCondition(generation: generation, deadline: deadline) {
                    !self.presentationJournal.hasSubmitted()
                }
            }
        } catch {
            // The VM is still running. Resume must establish the guest clock
            // before drawing is allowed again, even after a failed pause fence.
            // Cleanup gets its own task: the failed operation may already be
            // cancelled, but that must not cancel the rollback calibration.
            let rollback = Task { @MainActor [self] in
                try await resumePresentation(deadline: ProcessInfo.processInfo.systemUptime + 5)
                return CompletedPauseRollback(generation: connectionGeneration,
                                              sessionID: presentationSessionID)
            }
            completedPauseRollback = try? await rollback.value
            throw error
        }
    }

    /// Called only after VZ has resumed (or to roll back a failed pause).
    /// The guest acknowledges this after recalibrating its presentation clock.
    public func resumePresentationAfterVirtualMachinePause() async throws {
        guard !presentationBarrierInFlight else { throw WindowPresentationPauseError.busy }
        if let rollback = completedPauseRollback,
           rollback.generation == connectionGeneration,
           rollback.sessionID == presentationSessionID,
           !presentationDrawingGated, !presentationSuspended {
            // VMController also invokes its rollback hook after prepare fails.
            // Consume only this already completed rollback; a real pause,
            // restore or new session always requires a new clock epoch.
            completedPauseRollback = nil
            return
        }
        completedPauseRollback = nil
        presentationBarrierInFlight = true
        defer { presentationBarrierInFlight = false }
        try await resumePresentation(deadline: ProcessInfo.processInfo.systemUptime + 5)
    }

    private func resumePresentation(deadline: Double) async throws {
        if let sessionID = presentationSessionID {
            let token = newPresentationBarrierToken()
            try await requestPresentationBarrier(.resume, sessionID: sessionID,
                token: token, generation: connectionGeneration, deadline: deadline,
                command: .presentationResume(sessionID: sessionID, token: token))
        } else {
            try await waitForPresentationCondition(generation: connectionGeneration, deadline: deadline) {
                !self.presentationJournal.hasSubmitted()
            }
        }
        setSuspended(false)
        setPresentationDrawingGated(false)
        retryPendingFrames()
        refreshAuxiliaryFrames()
    }

    private func setPresentationDrawingGated(_ gated: Bool) {
        presentationDrawingGated = gated
        for native in windows.values { native.setPresentationSubmissionAllowed(!gated) }
        cursorPresenter?.setSubmissionAllowed(!gated)
        dragIconPresenter?.setSubmissionAllowed(!gated)
        updateAuxiliaryVisibility()
        if !gated {
            for surface in Array(deferredPresentationRefresh)
                where surface != cursorSurface && surface != dragIconSurface {
                flushDeferredPresentationRefresh(surface: surface)
            }
        }
    }

    private func newPresentationBarrierToken() -> UInt32 {
        nextPresentationBarrierToken &+= 1
        if nextPresentationBarrierToken == 0 { nextPresentationBarrierToken = 1 }
        return nextPresentationBarrierToken
    }

    private func reachPresentationBarrier(_ phase: PresentationBarrierPhase, sessionID: UInt64, token: UInt32) {
        guard presentationSessionID == sessionID,
              presentationBarrier?.sessionID == sessionID,
              presentationBarrier?.token == token,
              presentationBarrier?.phase == phase else { return }
        presentationBarrier?.reached = true
    }

    private func requestPresentationBarrier(
        _ phase: PresentationBarrierPhase, sessionID: UInt64, token: UInt32,
        generation: UInt64, deadline: Double, command: Windowing.HostCommand
    ) async throws {
        guard presentationSessionID == sessionID else { throw WindowPresentationPauseError.connectionChanged }
        presentationBarrier = PresentationBarrier(sessionID: sessionID, token: token, phase: phase)
        defer { presentationBarrier = nil }
        send(command)
        try await waitForPresentationCondition(generation: generation, deadline: deadline) {
            self.presentationBarrier?.reached == true
        }
        guard presentationSessionID == sessionID else { throw WindowPresentationPauseError.connectionChanged }
    }

    @MainActor private final class SubmissionFence {
        var remaining: Int
        init(_ count: Int) { remaining = count }
    }

    private func fencePresentationSubmission(generation: UInt64, deadline: Double) async throws {
        let fence = SubmissionFence(windows.count + 2)
        for native in windows.values {
            native.fencePresentationSubmission { fence.remaining -= 1 }
        }
        if let cursorPresenter { cursorPresenter.fenceSubmission { fence.remaining -= 1 } }
        else { fence.remaining -= 1 }
        if let dragIconPresenter { dragIconPresenter.fenceSubmission { fence.remaining -= 1 } }
        else { fence.remaining -= 1 }
        try await waitForPresentationCondition(generation: generation, deadline: deadline) {
            fence.remaining == 0
        }
        for native in windows.values { native.flushPresentationLatchesForPause() }
    }

    private func waitForPresentationCondition(
        generation: UInt64, deadline: Double, condition: () -> Bool
    ) async throws {
        while true {
            try Task.checkCancellation()
            guard connectionGeneration == generation else { throw WindowPresentationPauseError.connectionChanged }
            if condition() { return }
            guard ProcessInfo.processInfo.systemUptime < deadline else { throw WindowPresentationPauseError.timedOut }
            try await Task.sleep(for: .milliseconds(10))
        }
    }

    /// Authoritative mapped xdg_toplevels for the VM host's window switcher.
    /// Popups, cursor surfaces and drag icons never become application windows.
    public private(set) var computerSessionID = UUID()

    public var dockWindows: [DockWindow] {
        windows.values.compactMap { native -> DockWindow? in
            guard !native.isPopup, native.window != nil else { return nil }
            return DockWindow(
                id: native.windowID,
                title: native.title.isEmpty ? NPText("Untitled Window") : native.title,
                applicationID: native.applicationID,
                isMiniaturized: native.isMiniaturized,
                isZoomed: native.isZoomed,
                isFullscreen: native.isFullscreen,
                width: Double(native.window?.contentView?.bounds.width ?? 0),
                height: Double(native.window?.contentView?.bounds.height ?? 0),
                isVisible: native.window?.isVisible == true,
                isKey: native.window?.isKeyWindow == true,
                canForceQuit: forceQuitCapabilities[native.windowID] == true,
                frame: native.window?.frame,
                applicationName: native.applicationID.flatMap { applicationNameProvider?($0) })
        }.sorted {
            if $0.title == $1.title { return $0.id < $1.id }
            return $0.title.localizedStandardCompare($1.title) == .orderedAscending
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

    /// Capture NativePipe's own composed content. This never asks WindowServer
    /// to inspect another application's pixels, so no Screen Recording TCC is
    /// involved; title bars, shadows and neighbouring windows are absent by
    /// construction.
    public func captureDockWindow(
        _ id: UInt32, maximumWidth: Int, maximumHeight: Int
    ) async throws -> DockWindowCapture {
        guard maximumWidth > 0, maximumHeight > 0,
              let native = dockWindow(id), let window = native.window else {
            throw ComputerUseWindowError.unavailable
        }
        guard window.isVisible, !window.isMiniaturized else {
            throw ComputerUseWindowError.hidden
        }
        let contentSize = window.contentView?.bounds.size ?? .zero
        guard contentSize.width > 0, contentSize.height > 0 else {
            throw ComputerUseWindowError.captureFailed
        }
        let raw = try await native.captureFrame()
        return try await Task.detached(priority: .userInitiated) {
            try Self.encodeCapture(
                raw, contentSize: contentSize,
                maximumWidth: maximumWidth, maximumHeight: maximumHeight)
        }.value
    }

    private nonisolated static func encodeCapture(
        _ raw: RenderedFrameCapture, contentSize: CGSize,
        maximumWidth: Int, maximumHeight: Int
    ) throws -> DockWindowCapture {
        let (minimumBytesPerRow, rowOverflow) = raw.width
            .multipliedReportingOverflow(by: 4)
        let (requiredBytes, sizeOverflow) = raw.bytesPerRow
            .multipliedReportingOverflow(by: raw.height)
        guard raw.width > 0, raw.height > 0,
              !rowOverflow, !sizeOverflow,
              raw.bytesPerRow >= minimumBytesPerRow,
              raw.pixels.count >= requiredBytes,
              let provider = CGDataProvider(data: raw.pixels as CFData),
              let full = CGImage(
                width: raw.width, height: raw.height,
                bitsPerComponent: 8, bitsPerPixel: 32,
                bytesPerRow: raw.bytesPerRow,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGBitmapInfo(rawValue:
                    CGImageAlphaInfo.premultipliedFirst.rawValue |
                    CGBitmapInfo.byteOrder32Little.rawValue),
                provider: provider, decode: nil,
                shouldInterpolate: false, intent: .defaultIntent),
              let output = scaled(
                full, maximumWidth: maximumWidth,
                maximumHeight: maximumHeight) else {
            throw ComputerUseWindowError.captureFailed
        }
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(
            data, UTType.png.identifier as CFString, 1, nil) else {
            throw ComputerUseWindowError.captureFailed
        }
        CGImageDestinationAddImage(destination, output, nil)
        guard CGImageDestinationFinalize(destination) else {
            throw ComputerUseWindowError.captureFailed
        }
        return DockWindowCapture(
            png: data as Data,
            contentSize: contentSize,
            pixelSize: CGSize(width: output.width, height: output.height))
    }

    private nonisolated static func scaled(
        _ image: CGImage, maximumWidth: Int, maximumHeight: Int
    ) -> CGImage? {
        let ratio = min(
            1, CGFloat(maximumWidth) / CGFloat(image.width),
            CGFloat(maximumHeight) / CGFloat(image.height))
        guard ratio < 1 else { return image }
        let width = max(1, Int((CGFloat(image.width) * ratio).rounded(.down)))
        let height = max(1, Int((CGFloat(image.height) * ratio).rounded(.down)))
        guard let context = CGContext(
            data: nil, width: width, height: height,
            bitsPerComponent: 8, bytesPerRow: 0,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        context.interpolationQuality = .high
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        return context.makeImage()
    }

    @discardableResult
    public func computerPointerMove(window id: UInt32, x: Double, y: Double) -> Bool {
        guard acceptsKeyboardInput, x.isFinite, y.isFinite,
              let native = dockWindow(id), let view = native.window?.contentView,
              view.bounds.contains(CGPoint(x: x, y: y)) else { return false }
        let point = CGPoint(x: x, y: y)
        if computerPointerWindow != id {
            if let old = computerPointerWindow { windows[old]?.pointerLeft() }
            computerPointerWindow = id
            native.pointerEntered(at: point)
        }
        native.pointerMoved(to: point)
        return true
    }

    @discardableResult
    public func computerPointerButton(
        window id: UInt32, button: Windowing.PointerButton, pressed: Bool
    ) -> Bool {
        guard acceptsKeyboardInput, computerPointerWindow == id,
              let native = dockWindow(id) else { return false }
        native.pointerButton(button, pressed: pressed)
        return true
    }

    @discardableResult
    public func computerScroll(
        window id: UInt32, dx: Double, dy: Double, precise: Bool
    ) -> Bool {
        guard acceptsKeyboardInput, computerPointerWindow == id,
              dx.isFinite, dy.isFinite, let native = dockWindow(id) else { return false }
        native.pointerScroll(dx: dx, dy: dy, precise: precise)
        return true
    }

    @discardableResult
    public func computerKey(
        window id: UInt32, macKeyCode: UInt16, pressed: Bool,
        modifierFlags: UInt64
    ) -> Bool {
        guard acceptsKeyboardInput, KeyTranslation.evdevCode(for: macKeyCode) != nil, let native = dockWindow(id) else { return false }
        native.activateFromDock()
        guard native.window?.isKeyWindow == true, NSApp.isActive else { return false }
        native.key(
            macKeyCode, pressed: pressed,
            flags: NSEvent.ModifierFlags(rawValue: UInt(modifierFlags)))
        return true
    }

    @discardableResult
    public func computerCommitText(window id: UInt32, text: String) -> Bool {
        guard acceptsKeyboardInput, !text.isEmpty, text.utf8.count <= 65_535,
              !text.contains("\0"), let native = dockWindow(id) else { return false }
        native.activateFromDock()
        guard native.window?.isKeyWindow == true, NSApp.isActive, native.acceptsCommittedText else { return false }
        native.commitText(text)
        return true
    }

    @discardableResult
    public func setDockWindowFrame(_ id: UInt32, frame: CGRect) -> Bool {
        guard acceptsKeyboardInput, let native = dockWindow(id) else { return false }
        return native.setFrameFromControl(frame)
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
        guard dockWindow(id) != nil, forceQuitCapabilities[id] == true else {
            return false
        }
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
        notifyDockWindowsChanged()
    }

    func window(_ id: UInt32) -> NativeWindow? { windows[id] }

    // MARK: Guest activation

    private let activationAuthority = WindowActivationAuthority()

    private func activationOrigin(for id: UInt32) -> NativeWindow? {
        guard let native = windows[id] else { return nil }
        if !native.isPopup { return native }
        var parent = native.window?.parent
        for _ in 0..<64 {
            guard let current = parent else { return nil }
            if let origin = windows.values.first(where: { !$0.isPopup && $0.window === current }) {
                return origin
            }
            parent = current.parent
        }
        return nil
    }

    private func recordActivationInput(_ command: Windowing.HostCommand) {
        let id: UInt32
        switch command {
        case .key(let window, _, true, _), .pointerButton(let window, _, true): id = window
        case .keyboardFocus(nil): activationAuthority.clear(); return
        default: return
        }
        guard NSApp.isActive, let origin = activationOrigin(for: id),
              let window = origin.window, window.isKeyWindow else { return }
        activationAuthority.record(window: window, id: origin.windowID)
    }

    private func activateGuestWindow(_ id: UInt32, originID: UInt32, inputAge: UInt32) {
        guard let origin = windows[originID]?.window,
              let target = windows[id], !target.isPopup, target.window != nil,
              activationAuthority.consume(
                window: origin, id: originID, guestInputAgeMilliseconds: inputAge,
                appIsActive: NSApp.isActive, originIsKey: origin.isKeyWindow)
        else { return }
        target.activateFromDock()
    }

    func currentPointerCursor() -> NSCursor {
        cursorUsesSoftwarePresentation && cursorSurface != nil && auxiliaryPointerVisible
            ? transparentPointerCursor : pointerCursor
    }

    private var auxiliaryPointerVisible: Bool {
        guard !presentationDrawingGated, !presentationSuspended, NSApp.isActive,
              let id = pointerPresentationWindow, let native = windows[id],
              native.canPresent else { return false }
        return true
    }

    func pointerPresentationEntered(window: UInt32, position: CGPoint) {
        pointerPresentationWindow = window
        pointerPresentationPosition = position
        updateAuxiliaryVisibility()
        refreshPointerCursor()
        refreshAuxiliaryFrames()
    }

    func pointerPresentationMoved(window: UInt32, position: CGPoint) {
        let entered = pointerPresentationWindow != window
        pointerPresentationWindow = window
        pointerPresentationPosition = position
        updateAuxiliaryVisibility()
        if entered { refreshPointerCursor(); refreshAuxiliaryFrames() }
    }

    func pointerPresentationLeft(window: UInt32) {
        guard pointerPresentationWindow == window else { return }
        pointerPresentationWindow = nil
        updateAuxiliaryVisibility()
        refreshPointerCursor()
    }

    func pointerPresentationVisibilityChanged(window: UInt32) {
        guard pointerPresentationWindow == window else { return }
        let wasVisible = auxiliaryWasVisible
        updateAuxiliaryVisibility()
        if wasVisible != auxiliaryWasVisible {
            refreshPointerCursor()
            if auxiliaryWasVisible { refreshAuxiliaryFrames() }
        }
    }

    private func updateAuxiliaryVisibility() {
        let visible = auxiliaryPointerVisible
        let wasVisible = auxiliaryWasVisible
        auxiliaryWasVisible = visible
        let cursorVisible = visible && cursorSurface != nil && cursorUsesSoftwarePresentation
        if cursorVisible { cursorPresenter?.move(to: pointerPresentationPosition, hotSpot: cursorPresentationHotSpot) }
        if visible, dragIconSurface != nil, !dragExportSuppressed { dragIconPresenter?.move(to: pointerPresentationPosition) }
        cursorPresenter?.setVisible(cursorVisible)
        dragIconPresenter?.setVisible(visible && dragIconSurface != nil && !dragExportSuppressed)
        if wasVisible, !visible {
            if cursorPublicationIsQueried, let surface = cursorSurface { retireAuxiliaryFrames(surface: surface) }
            if let surface = dragIconSurface { retireAuxiliaryFrames(surface: surface) }
        }
    }

    private func refreshAuxiliaryFrames() {
        guard auxiliaryPointerVisible else { return }
        for surface in [cursorSurface, dragExportSuppressed ? nil : dragIconSurface].compactMap({ $0 }) {
            if deferredPresentationRefresh.contains(surface) {
                flushDeferredPresentationRefresh(surface: surface)
                continue
            }
            if let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
                apply(.committed(surface: surface, frame: frame))
            } else if pendingFrames[surface] == nil,
                      surface != cursorSurface || cursorUsesSoftwarePresentation || cursorPublicationIsQueried {
                // Only a new protected publication may redraw a cursor whose
                // previous source read has already been released to Linux.
                send(.captureFrame(surface: surface))
            }
        }
    }

    private func requestAuxiliaryPublication(surface: UInt32) {
        guard auxiliaryPointerVisible, pendingFrames[surface] == nil,
              pendingSurfaceFrames[surface] == nil else { return }
        if surface == cursorSurface {
            guard cursorUsesSoftwarePresentation else { return }
        } else {
            guard surface == dragIconSurface, !dragExportSuppressed else { return }
        }
        send(.captureFrame(surface: surface))
    }

    private func refreshPointerCursor() {
        for window in windows.values { window.refreshPointerCursor() }
        if pointerPresentationWindow != nil, NSApp.isActive { currentPointerCursor().set() }
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
		guard !presentationSuspended else { return }
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
        pointerPresentationLeft(window: windowID)
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
			native.reportWindowState()
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
			// CGDisplayPixelsWide/High report mode points on a HiDPI display.
			// wl_output.mode needs backing pixels before wl_output.scale divides it.
			let mode = CGDisplayCopyDisplayMode(directID)
			let pixelWidth = mode?.pixelWidth ?? Int((frame.width * CGFloat(scale)).rounded())
			let pixelHeight = mode?.pixelHeight ?? Int((frame.height * CGFloat(scale)).rounded())
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
        if case .captureFrame(let surface) = command {
            if hasDiscardAwaitingAcknowledgement(surface: surface) {
                // Feedback and control use independent writers. A republished
                // current commit must wait until Linux has rebound its query and
                // acknowledged the failed display attempt.
                deferredPresentationRefresh.insert(surface)
                return
            }
            deferredPresentationRefresh.remove(surface)
        }
        recordActivationInput(command)
        if case .pointerButton(_, _, false) = command { fileDrag.pointerReleased() }
        // Outgoing commands were the one direction with no trace, which made
        // "input does not work" impossible to localise from the logs alone.
        switch command {
        case .pointerMoved:
            break  // every frame of mouse movement would drown everything else
        case .pointerScroll:
            break  // trackpads can report hundreds per second
        case .configure(_, _, let states, _) where states.contains(.resizing):
            if Self.frameTrace { Self.note("-> \(command)") }
        case .pointerEntered:
            Self.note("-> \(command)")
        default:
            Self.note("-> \(command)")
        }
        output?(command)
    }

    // MARK: - Event application

    public func apply(_ event: Windowing.GuestEvent) {
        if case .fileDrag(let message) = event {
            if message.action == .offered { dragExportSuppressed = false; updateAuxiliaryVisibility() }
            fileDrag.receive(message)
            return
        }
        if case .committed(let surface, let frame) = event {
            trackAuxiliaryFrame(frame, surface: surface)
            removePendingAuxiliaryFrames(surface: surface, except: frame)
        }
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
        case .hostOpenRequested, .hostOpenCancelled, .hostOpenRejected: break // owned by the remote host-open coordinator
        case .fileDrag: break // handled above, before rendering
        case .notificationPosted(let notification): onGuestNotification?(notification)
        case .notificationClosed(let id, let revision): onGuestNotificationClosed?(id, revision)
        case .notificationRejected(let id, let revision):
            if let id, let revision {
                onGuestNotificationClosed?(id, revision)
                output?(.notificationClosed(id: id, revision: revision, reason: .undefined))
            }
        case .notificationBacklogReset: onGuestNotificationBacklogReset?()
        case .presentationClockRequested(let token, let sessionID, let clockEpoch):
            if reportsPresentationTime {
                acceptPresentationSession(sessionID)
                let nanoseconds = UInt64((CACurrentMediaTime() * 1_000_000_000).rounded())
                send(.presentationClockSample(token: token, sessionID: sessionID,
                     clockEpoch: clockEpoch, hostTimeNanoseconds: nanoseconds))
            }
        case .presentationFeedbackAcknowledged(let sessionID, let clockEpoch, let surface, let presentationID):
            let key = PresentationJournal.Key(sessionID: sessionID, clockEpoch: clockEpoch,
                                              surface: surface, presentationID: presentationID)
            presentationJournal.acknowledge(key)
            sentPresentationResults.remove(key)
            flushDeferredPresentationRefresh(surface: surface)
        case .presentationPauseReached(let sessionID, let token):
            reachPresentationBarrier(.pause, sessionID: sessionID, token: token)
        case .presentationDrained(let sessionID, let token):
            reachPresentationBarrier(.drain, sessionID: sessionID, token: token)
        case .presentationResumed(let sessionID, _, let token):
            reachPresentationBarrier(.resume, sessionID: sessionID, token: token)
        case .channelReady:
            activationAuthority.clear()
            onChannelReady?()
            onGuestFileSharingRevoked?()
            connectionGeneration &+= 1
            presentationSessionID = nil
            completedPauseRollback = nil
            sentPresentationResults.removeAll()
            deferredPresentationRefresh.removeAll()
            // Consumed by WindowChannel as the transport generation boundary.
			lastDisplays.removeAll(keepingCapacity: true)
			publishDisplayTopology(force: true)
            sendInputPreferences()
            clipboard.connectionReady()
            break

        case .surfaceCreated(let surface):
            knownSurfaces.insert(surface)

        case .surfaceDestroyed(let surface):
            knownSurfaces.remove(surface)
            deferredPresentationRefresh.remove(surface)
            if let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
                completeAuxiliaryFrame(frame, surface: surface)
            }
            if let frame = pendingFrames.removeValue(forKey: surface) {
                completeAuxiliaryFrame(frame, surface: surface)
            }
            cancelSceneWork(for: surface)
            if dragIconSurface == surface {
                dragIconSurface = nil
                dragExportSuppressed = false
                dragIconPresenter?.resetContent()
            }
            if cursorSurface == surface {
                cursorSurface = nil
                cursorGeometryFrame = nil
                cursorUsesSoftwarePresentation = false
                cursorPublicationIsQueried = false
                cursorPresenter?.resetContent()
                pointerCursor = .arrow
                refreshPointerCursor()
            }
            if let windowID = surfaceToWindow.removeValue(forKey: surface) {
                mappedApplicationWindows.remove(windowID)
                if computerPointerWindow == windowID { computerPointerWindow = nil }
                windows.removeValue(forKey: windowID)?.close()
            }

        case .surfaceUnmapped(let surface):
            deferredPresentationRefresh.remove(surface)
            if let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
                completeAuxiliaryFrame(frame, surface: surface)
            }
            if let frame = pendingFrames.removeValue(forKey: surface) {
                completeAuxiliaryFrame(frame, surface: surface)
            }
            cancelSceneWork(for: surface)
            if cursorSurface == surface {
                cursorGeometryFrame = nil
                cursorUsesSoftwarePresentation = false
                cursorPublicationIsQueried = false
                pointerCursor = .arrow
                cursorPresenter?.resetContent()
                refreshPointerCursor()
            }
            if dragIconSurface == surface { dragIconPresenter?.resetContent() }
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

        case .forceQuitCapabilityChanged(let window, let supported):
            guard windows[window] != nil else { break }
            forceQuitCapabilities[window] = supported
            notifyDockWindowsChanged()

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
            forceQuitCapabilities.removeValue(forKey: window)
            if let native = windows.removeValue(forKey: window) {
                if computerPointerWindow == window { computerPointerWindow = nil }
                mappedApplicationWindows.remove(window)
                surfaceToWindow.removeValue(forKey: native.surfaceID)
                native.close()
            }

        case .dragIconChanged(let surface):
            if let old = dragIconSurface, old != surface { retireAuxiliaryFrames(surface: old) }
            if dragIconSurface != surface { dragIconPresenter?.resetContent() }
            dragIconSurface = surface
            if surface == nil { dragExportSuppressed = false }
            updateAuxiliaryVisibility()
            guard let surface else {
                dragIconPresenter?.setVisible(false)
                return
            }
            if let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
                guard let texture = texture(for: frame) else {
                    retainDeferred(frame, for: surface)
                    return
                }
                presentDragIcon(texture, frame: frame, surface: surface)
            }

        case .cursorChanged(let surface, let hotspotX, let hotspotY, let pixelScale):
            if let old = cursorSurface, old != surface { retireAuxiliaryFrames(surface: old) }
            if cursorSurface != surface {
                cursorGeometryFrame = nil
                cursorUsesSoftwarePresentation = false
                cursorPublicationIsQueried = false
                pointerCursor = .arrow
                cursorPresenter?.resetContent()
            }
            cursorSurface = surface
            cursorHotSpot = CGPoint(x: hotspotX, y: hotspotY)
            cursorPixelScale = pixelScale
            if let frame = cursorGeometryFrame {
                let geometry = Self.customCursorGeometry(
                    frame: frame, hotSpot: cursorHotSpot, pixelScale: pixelScale)
                cursorPresentationHotSpot = geometry.hotSpot
                if !cursorUsesSoftwarePresentation, let image = pointerCursor.image.copy() as? NSImage {
                    image.size = geometry.imageSize
                    pointerCursor = NSCursor(image: image, hotSpot: geometry.hotSpot)
                }
            }
            updateAuxiliaryVisibility()
            refreshPointerCursor()
            guard let surface else {
                pointerCursor = .arrow
                refreshPointerCursor()
                break
            }
            if let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
                installCustomCursor(frame, surface: surface)
            } else if auxiliaryPointerVisible, pendingFrames[surface] == nil {
                send(.captureFrame(surface: surface))
            }

        case .cursorShapeChanged(let shape):
            if let old = cursorSurface { retireAuxiliaryFrames(surface: old) }
            cursorSurface = nil
            cursorGeometryFrame = nil
            cursorUsesSoftwarePresentation = false
            cursorPublicationIsQueried = false
            cursorPresenter?.resetContent()
            pointerCursor = NativeCursorResolver.cursor(for: shape)
            refreshPointerCursor()

        case .titleChanged(let window, let title):
            windows[window]?.title = title
            notifyDockWindowsChanged()

        case .appIDChanged(let window, let appID):
            windows[window]?.setAppID(appID)
            notifyApplicationWindowMapped(window)
            notifyDockWindowsChanged()

        case .decorationModeChanged(let window, let serverSide):
            windows[window]?.setServerDecorated(serverSide)

        case .parentChanged(let window, let parent):
            windows[window]?.setParent(parent.flatMap { windows[$0] })

        case .sizeConstraintsChanged(let window, let minimum, let maximum):
            windows[window]?.setConstraints(minimum: minimum, maximum: maximum)

        case .committed(let surface, let frame):
            if presentationDrawingGated {
                completeAuxiliaryFrame(frame, surface: surface)
                return
            }
            if surface == cursorSurface {
                installCustomCursor(frame, surface: surface)
                return
            }
            if surface == dragIconSurface {
                if dragExportSuppressed { completeAuxiliaryFrame(frame, surface: surface); return }
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
            windows[window]?.beginInteractiveMove()

        case .interactiveResizeRequested:
            // Both server-decorated and borderless CSD windows carry AppKit's
            // .resizable style, so the real window edge owns the resize loop.
            // Manually changing frames here competes with AppKit hit testing.
            break

        case .fullscreenRequested(let window, let enabled):
            windows[window]?.setFullscreen(enabled)

        case .maximizeRequested(let window, let enabled):
            windows[window]?.setMaximized(enabled)

        case .activationRequested(let window, let origin, let age):
            activateGuestWindow(window, originID: origin, inputAge: age)

        case .textInputEnabled(let window, let epoch, let enabled):
            windows[window]?.setTextInput(enabled: enabled, epoch: epoch)

        case .textInputCursorRect(let window, let x, let y, let width, let height):
            windows[window]?.setTextCursorRect(
                CGRect(x: x, y: y, width: max(width, 1), height: max(height, 1)))

        case .textInputSurroundingText(let window, let text, let cursor, let anchor):
            windows[window]?.setTextSurrounding(text, cursor: cursor, anchor: anchor)

        case .textInputContentType(let window, let hints, let purpose, let cause):
            windows[window]?.setTextContentType(hints: hints, purpose: purpose, changeCause: cause)

        case .selectionOffered(let mimeTypes):
            clipboard.guestOffered(mimeTypes: mimeTypes)

        case .selectionData(let token, _, let data):
            clipboard.guestSuppliedData(token: token, data: data)

        case .hostSelectionRequest(let token, let mimeType):
            clipboard.guestRequestedHostData(token: token, mimeType: mimeType)

        case .minimizeRequested(let window):
            windows[window]?.window?.miniaturize(nil)
        }
    }

    /// Called when a virtio-gpu resource becomes presentable after CREATE_BLOB.
    public func retryPendingFrames() {
        guard !presentationSuspended, !presentationDrawingGated else { return }
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
        let record: PresentationRecord?
        if reportsPresentationTime, scene.presentationID != 0,
           let context = scene.presentationContext {
            acceptPresentationSession(context.sessionID)
            record = presentationJournal.register(.init(
                sessionID: context.sessionID, clockEpoch: context.clockEpoch,
                surface: scene.surface, presentationID: scene.presentationID))
            send(.sceneClockSample(sessionID: context.sessionID, clockEpoch: context.clockEpoch,
                 surface: scene.surface, presentationID: scene.presentationID,
                 guestSendTimeNanoseconds: context.guestSendTimeNanoseconds,
                 hostReceiveTimeNanoseconds: scene.receivedHostTimeNanoseconds ??
                    UInt64((CACurrentMediaTime() * 1_000_000_000).rounded())))
        } else { record = nil }
        var work = SceneWork(scene: scene, record: record)
        if presentationDrawingGated {
            complete(work)
            return
        }
        if let older = pendingScenes[scene.surface] {
            work = work.superseding(older)
            releaseScene(older.scene)
            older.record?.discardIfUnsubmitted()
            scenePresented(surface: older.scene.surface, presentationID: older.scene.presentationID, displayed: false)
        }
        pendingScenes[scene.surface] = work
    }

    private func startPendingScene(for surface: UInt32) {
        guard !presentationSuspended, !presentationDrawingGated,
              resolvingScenes[surface] == nil,
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

        // Prefer the newest complete scene. New metadata may precede decode;
        // continually discarding ready pixels for that metadata starves display.
        let ready = results.count == work.scene.layers.count && results.allSatisfy { $0.status == .ready }
        if var newer = pendingScenes[surface], !ready || presentationSuspended ||
            newer.scene.layers.allSatisfy({ frameSource?.isResourcePublished($0.resourceID) == true }) {
            pendingScenes.removeValue(forKey: surface)
            newer = newer.superseding(work)
            releaseScene(work.scene)
            work.record?.discardIfUnsubmitted()
            scenePresented(surface: work.scene.surface, presentationID: work.scene.presentationID, displayed: false)
            pendingScenes[surface] = newer
            startPendingScene(for: surface)
            return
        }

        if presentationDrawingGated {
            complete(work)
            return
        }
        if presentationSuspended {
            pendingScenes[surface] = work
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
                layers.append(ResolvedSceneLayer(state: state, texture: texture, owner: result.owner))
            case .unpublished:
                unpublished.append(state.resourceID)
            case .unavailable:
                unavailable.append(state.resourceID)
            }
        }
        if !unavailable.isEmpty {
            discardScene(
                work,
                reason: NPText("resources %@ cannot export their committed Metal textures", String(describing: unavailable)))
            return
        }
        guard unpublished.isEmpty else {
            pendingScenes[surface] = work
            Self.note("scene deferred: waiting for resources \(unpublished)")
            return
        }

        let generation = connectionGeneration
        guard let windowID = surfaceToWindow[surface],
              let native = windows[windowID],
              native.present(
                scene: work.scene, layers: layers, latchIDs: work.latchIDs,
                record: work.record,
                readComplete: { [weak self] _ in
                    guard self?.connectionGeneration == generation else { return }
                    self?.releaseScene(work.scene)
                })
        else {
            discardScene(work, reason: NPText("window presenter rejected the scene"))
            return
        }

        startPendingScene(for: surface)
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
        work.record?.discardIfUnsubmitted()
        scenePresented(surface: work.scene.surface, presentationID: work.scene.presentationID, displayed: false)
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
        work.record?.discardIfUnsubmitted()
        scenePresented(surface: work.scene.surface, presentationID: work.scene.presentationID, displayed: false)
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

    private func resolveFrame(_ frame: Windowing.Frame) -> FrameTextureResolution? {
        let format: UInt32 = frame.format == .rgba8888 ? 67 : 1
        return frameSource?.resolveFrame(
            forResource: frame.resourceID,
            width: frame.width, height: frame.height,
            bytesPerRow: frame.bytesPerRow, format: format)
    }

    private func texture(for frame: Windowing.Frame) -> (texture: MTLTexture, owner: AnyObject?)? {
        guard let resolved = resolveFrame(frame),
              let texture = resolved.texture as? MTLTexture else { return nil }
        return (texture, resolved.owner)
    }

    private func auxiliaryKey(_ frame: Windowing.Frame, surface: UInt32) -> PresentationJournal.Key? {
        guard reportsPresentationTime, frame.requestsPresentationFeedback, frame.presentationID != 0,
              let context = frame.presentationContext else { return nil }
        return .init(sessionID: context.sessionID, clockEpoch: context.clockEpoch,
                     surface: surface, presentationID: frame.presentationID)
    }

    private func trackAuxiliaryFrame(_ frame: Windowing.Frame, surface: UInt32) {
        guard let key = auxiliaryKey(frame, surface: surface),
              auxiliaryRecords[key] == nil, let context = frame.presentationContext else { return }
        acceptPresentationSession(context.sessionID)
        auxiliaryRecords[key] = presentationJournal.register(key)
        send(.sceneClockSample(sessionID: context.sessionID, clockEpoch: context.clockEpoch,
             surface: surface, presentationID: frame.presentationID,
             guestSendTimeNanoseconds: context.guestSendTimeNanoseconds,
             hostReceiveTimeNanoseconds: frame.receivedHostTimeNanoseconds ??
                UInt64((CACurrentMediaTime() * 1_000_000_000).rounded())))
    }

    private func takeAuxiliaryRecord(_ frame: Windowing.Frame, surface: UInt32) -> PresentationRecord? {
        guard let key = auxiliaryKey(frame, surface: surface) else { return nil }
        return auxiliaryRecords.removeValue(forKey: key)
    }

    private func completeAuxiliaryFrame(_ frame: Windowing.Frame, surface: UInt32) {
        takeAuxiliaryRecord(frame, surface: surface)?.discardIfUnsubmitted()
        completeCopiedPresentation(surface: surface, presentationID: frame.presentationID)
    }

    private func retireAuxiliaryFrames(surface: UInt32) {
        if let frame = pendingSurfaceFrames.removeValue(forKey: surface) {
            completeAuxiliaryFrame(frame, surface: surface)
        }
        if let frame = pendingFrames.removeValue(forKey: surface) {
            completeAuxiliaryFrame(frame, surface: surface)
        }
    }

    /// A surface has one protected publication waiting for either a role or a
    /// resource. Moving between those reasons must not create two latest slots.
    private func removePendingAuxiliaryFrames(surface: UInt32, except frame: Windowing.Frame) {
        for previous in [pendingSurfaceFrames.removeValue(forKey: surface),
                         pendingFrames.removeValue(forKey: surface)].compactMap({ $0 }) {
            if previous.presentationID != frame.presentationID ||
                previous.presentationContext != frame.presentationContext {
                completeAuxiliaryFrame(previous, surface: surface)
            }
        }
    }

    private func installCustomCursor(_ frame: Windowing.Frame, surface: UInt32) {
        cursorPublicationIsQueried = frame.requestsPresentationFeedback
        if !frame.requestsPresentationFeedback {
            installNativeCustomCursor(frame, surface: surface)
            return
        }
        guard auxiliaryPointerVisible else { retainUnroled(frame, for: surface); return }
        guard let resolved = texture(for: frame) else { retainDeferred(frame, for: surface); return }
        let geometry = Self.customCursorGeometry(frame: frame, hotSpot: cursorHotSpot,
                                                  pixelScale: cursorPixelScale)
        cursorGeometryFrame = frame
        cursorUsesSoftwarePresentation = true
        cursorPresentationHotSpot = geometry.hotSpot
        customCursor.move(to: pointerPresentationPosition, hotSpot: geometry.hotSpot)
        customCursor.setVisible(true)
        presentAuxiliary(resolved, frame: frame, surface: surface,
                         logicalSize: geometry.imageSize, presenter: customCursor)
        refreshPointerCursor()
    }

    /// Ordinary custom cursors retain AppKit's low-latency pointer path. Only
    /// commits with an actual presentation query need a measurable drawable.
    private func installNativeCustomCursor(_ frame: Windowing.Frame, surface: UInt32) {
        guard let resolved = texture(for: frame) else { retainDeferred(frame, for: surface); return }
        let texture = resolved.texture
        defer { withExtendedLifetime(resolved.owner) {} }
        let geometry = Self.customCursorGeometry(
            frame: frame, hotSpot: cursorHotSpot, pixelScale: cursorPixelScale)
        let source = geometry.sourcePixels.intersection(
            CGRect(x: 0, y: 0, width: texture.width, height: texture.height))
        guard !source.isEmpty,
              let image = CIImage(mtlTexture: texture, options: [
                .colorSpace: CGColorSpaceCreateDeviceRGB()
              ]) else { completeAuxiliaryFrame(frame, surface: surface); return }
        if cursorContext == nil { cursorContext = CIContext(mtlDevice: texture.device) }
        let ciSource = CGRect(x: source.minX, y: CGFloat(texture.height) - source.maxY,
            width: source.width, height: source.height)
        let cursorImage = Self.topDownCursorImage(image, source: ciSource)
        guard let cgImage = cursorContext?.createCGImage(cursorImage, from: ciSource,
            format: .RGBA8, colorSpace: CGColorSpace(name: CGColorSpace.sRGB), deferred: false) else {
            completeAuxiliaryFrame(frame, surface: surface)
            return
        }
        cursorGeometryFrame = frame
        pointerCursor = NSCursor(image: NSImage(cgImage: cgImage, size: geometry.imageSize),
            hotSpot: geometry.hotSpot)
        cursorUsesSoftwarePresentation = false
        cursorPresenter?.resetContent()
        updateAuxiliaryVisibility()
        refreshPointerCursor()
        completeAuxiliaryFrame(frame, surface: surface)
    }

    private func presentDragIcon(
        _ resolved: (texture: MTLTexture, owner: AnyObject?), frame: Windowing.Frame, surface: UInt32
    ) {
        if dragExportSuppressed { completeAuxiliaryFrame(frame, surface: surface); return }
        guard auxiliaryPointerVisible else { retainUnroled(frame, for: surface); return }
        dragIcon.move(to: pointerPresentationPosition)
        dragIcon.setVisible(true)
        let scale = CGFloat(max(frame.scale, 1))
        presentAuxiliary(resolved, frame: frame, surface: surface,
            logicalSize: CGSize(width: CGFloat(frame.width) / scale, height: CGFloat(frame.height) / scale),
            presenter: dragIcon)
    }

    private func presentAuxiliary(
        _ resolved: (texture: MTLTexture, owner: AnyObject?), frame: Windowing.Frame,
        surface: UInt32, logicalSize: CGSize, presenter: AuxiliarySurfacePresenter
    ) {
        let record = takeAuxiliaryRecord(frame, surface: surface)
        guard let renderer = sceneRenderer(for: resolved.texture.device) else {
            record?.discardIfUnsubmitted()
            completeCopiedPresentation(surface: surface, presentationID: frame.presentationID)
            return
        }
        pendingSurfaceFrames.removeValue(forKey: surface)
        pendingFrames.removeValue(forKey: surface)
        let generation = connectionGeneration
        let queued = presenter.present(texture: resolved.texture, owner: resolved.owner,
            frame: frame, surface: surface, logicalSize: logicalSize, renderer: renderer, record: record,
            readComplete: { [weak self] _ in
                guard let self, self.connectionGeneration == generation else { return }
                self.send(.frameReleased(surface: surface, presentationID: frame.presentationID))
            }, latched: { [weak self] in
                guard self?.connectionGeneration == generation else { return }
                self?.send(.framePresented(surface: surface, presentationID: frame.presentationID))
            })
        if !queued {
            record?.discardIfUnsubmitted()
            completeCopiedPresentation(surface: surface, presentationID: frame.presentationID)
        }
    }

    /// Replace a frame the host has not installed only after completing the
    /// superseded presentation id. Once a commit crosses the guest/host
    /// boundary, that id retains its source texture until frameReleased.
    private func retainDeferred(_ frame: Windowing.Frame, for surface: UInt32) {
        removePendingAuxiliaryFrames(surface: surface, except: frame)
        pendingFrames[surface] = frame
    }

    /// The same rule applies to a commit that arrives before its xdg role.
    private func retainUnroled(_ frame: Windowing.Frame, for surface: UInt32) {
        removePendingAuxiliaryFrames(surface: surface, except: frame)
        pendingSurfaceFrames[surface] = frame
    }

    private func presentCommitted(surface: UInt32, windowID: UInt32, frame: Windowing.Frame) {
        guard let native = windows[windowID] else {
            Self.note("commit dropped: no window \(windowID)")
            return
        }
        if frame.presentationContext != nil {
            // Flat publications precede cursor/drag role assignment. Once a
            // window owns this surface it needs a fresh composed scene, whose
            // drawable provides the real timestamp; CALayer installation has
            // no equivalent public display callback.
            completeAuxiliaryFrame(frame, surface: surface)
            send(.captureFrame(surface: surface))
            return
        }
        guard frame.source == .encoded else {
            completeAuxiliaryFrame(frame, surface: surface)
            Self.note("ignored obsolete local committed frame for surface \(surface)")
            return
        }
        guard !presentationSuspended, !presentationDrawingGated else {
            retainDeferred(frame, for: surface)
            return
        }

        guard let resolved = resolveFrame(frame), let ioSurface = resolved.surface else {
            retainDeferred(frame, for: surface)
            Self.note("remote frame deferred: no decoded IOSurface for resource \(frame.resourceID)")
            return
        }
        pendingFrames.removeValue(forKey: surface)
        native.present(frame: frame, surface: ioSurface, owner: resolved.owner)
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
        activationAuthority.clear()
        computerSessionID = UUID()
        connectionGeneration &+= 1
        presentationSessionID = nil
        completedPauseRollback = nil
        sentPresentationResults.removeAll()
        presentationBarrier = nil
        fileDrag.disconnect()
        clipboard.disconnect()
        onGuestFileSharingRevoked?()
        for (surface, frame) in pendingSurfaceFrames {
            completeAuxiliaryFrame(frame, surface: surface)
        }
        pendingSurfaceFrames.removeAll()
        for (surface, frame) in pendingFrames {
            completeAuxiliaryFrame(frame, surface: surface)
        }
        pendingFrames.removeAll()
        for work in pendingScenes.values { complete(work) }
        pendingScenes.removeAll()
        for resolving in resolvingScenes.values { complete(resolving.work) }
        resolvingScenes.removeAll()
        cursorSurface = nil
        cursorGeometryFrame = nil
        cursorUsesSoftwarePresentation = false
        cursorPublicationIsQueried = false
        cursorContext = nil
        cursorPresentationHotSpot = .zero
        pointerPresentationWindow = nil
        auxiliaryWasVisible = false
        dragIconSurface = nil
        dragExportSuppressed = false
        cursorPresenter?.close()
        dragIconPresenter?.close()
        auxiliaryRecords.values.forEach { $0.discardIfUnsubmitted() }
        auxiliaryRecords.removeAll()
        deferredPresentationRefresh.removeAll()
        pointerCursor = .arrow
        for (_, window) in windows { window.close() }
        windows.removeAll()
        presentationJournal.discardUnsubmitted()
        deferredSceneFeedback.reset()
		forceQuitCapabilities.removeAll(keepingCapacity: true)
		windowDisplayStates.removeAll(keepingCapacity: true)
        popupPlacements.removeAll()
        mappedApplicationWindows.removeAll()
        surfaceToWindow.removeAll()
        knownSurfaces.removeAll()
        presentationSuspended = false
        presentationDrawingGated = false
        suspendedVisibleWindows.removeAll(keepingCapacity: true)
        suspendedKeyWindow = nil
        computerPointerWindow = nil
        notifyDockWindowsChanged()
    }
}
