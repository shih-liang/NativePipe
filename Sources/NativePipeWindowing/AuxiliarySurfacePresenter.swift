import AppKit
@preconcurrency import Metal
import NativePipeProtocol
import QuartzCore

/// Cursor and drag pixels use their own click-through drawable window. AppKit
/// still owns pointer tracking, hit testing and the external file-drag session.
@MainActor
final class AuxiliarySurfacePresenter: NSObject, DisplayClockTarget, NSWindowDelegate {
    enum Kind { case cursor, drag }

    private let kind: Kind
    private let displayClock: DisplayClock
    private let panel: NSPanel
    private let view = NSView()
    private(set) var metalLayer: CAMetalLayer
    private let output = PresentationOutputState()
    private var presenter: AsyncMetalScenePresenter?
    private var presenterRenderer: HostSceneRenderer?
    private var retiredPresenters: [ObjectIdentifier: AsyncMetalScenePresenter] = [:]
    private var visible = false
    private var submissionAllowed = true
    private var pointer = CGPoint.zero
    private var hotSpot = CGPoint.zero
    private var logicalSize = CGSize.zero
    private var needsDisplayRetry = false
    private var publicationGeneration: UInt64 = 0
    private var publication: (surface: UInt32, presentationID: UInt32)?
    private var needsNewPublication = false
    private var pendingLatches: [@MainActor @Sendable () -> Void] = []
    var onNeedsNewPublication: ((UInt32, UInt32) -> Void)?

    init(kind: Kind, displayClock: DisplayClock, metalLayer: CAMetalLayer = CAMetalLayer()) {
        self.kind = kind
        self.displayClock = displayClock
        self.metalLayer = metalLayer
        panel = NSPanel(contentRect: .zero,
            styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        super.init()
        Self.configure(metalLayer)
        view.wantsLayer = true
        view.layer = metalLayer
        panel.contentView = view
        panel.delegate = self
        panel.isReleasedWhenClosed = false
        panel.isExcludedFromWindowsMenu = true
        panel.backgroundColor = .clear
        panel.isOpaque = false
        panel.hasShadow = false
        panel.ignoresMouseEvents = true
        panel.hidesOnDeactivate = false
        panel.level = .popUpMenu
        panel.animationBehavior = .none
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
    }

    @discardableResult
    func present(
        texture: MTLTexture, owner: AnyObject?, frame: Windowing.Frame,
        surface: UInt32, logicalSize: CGSize, renderer: HostSceneRenderer,
        record: PresentationRecord?,
        readComplete: @escaping @MainActor @Sendable (Bool) -> Void,
        latched: @escaping @MainActor @Sendable () -> Void
    ) -> Bool {
        guard surface != 0, logicalSize.width.isFinite, logicalSize.height.isFinite,
              logicalSize.width > 0, logicalSize.height > 0,
              logicalSize.width <= 32_768, logicalSize.height <= 32_768 else { return false }
        if let previous = presenterRenderer, previous !== renderer { resetContent() }
        self.logicalSize = logicalSize
        panel.setContentSize(logicalSize)
        updatePosition()
        metalLayer.frame = view.bounds
        let screen = panel.screen ?? NSScreen.screens.first { $0.frame.contains(pointer) } ?? NSScreen.main
        let backingScale = screen?.backingScaleFactor ?? 1
        guard let geometry = Self.drawingGeometry(frame: frame, surface: surface,
            logicalSize: logicalSize, backingScale: backingScale) else { return false }
        metalLayer.contentsScale = backingScale
        if presenter == nil {
            presenter = AsyncMetalScenePresenter(layer: metalLayer,
                device: texture.device, renderer: renderer,
                requestDisplayRetry: { [weak self] in
                    guard let owner = self else { return }
                    MainRunLoop.perform { [weak owner] in owner?.needsDisplayRetry = true }
                })
            presenterRenderer = renderer
        }
        guard let presenter else { return false }
        presenter.setSubmissionAllowed(submissionAllowed)
        presenter.setDrawableAllowed(visible)
        if visible { panel.orderFrontRegardless() }
        updateClock()
        updateOutput()
        record?.useOutput(output)
        publicationGeneration &+= 1
        let generation = publicationGeneration
        publication = (surface, frame.presentationID)
        needsNewPublication = false
        let layer = geometry.scene.layers[0]
        presenter.enqueue(scene: geometry.scene,
            layers: [.init(state: layer, texture: texture, owner: owner)],
            drawableSize: geometry.drawableSize,
            readComplete: readComplete,
            latched: { [weak self] in
                guard let self else { latched(); return }
                if self.visible { self.pendingLatches.append(latched) }
                else { latched() }
            }, presentationTime: { [weak self] time in
                guard let self, time == nil, self.publicationGeneration == generation,
                      self.visible, self.submissionAllowed,
                      record == nil || record?.hasDiscardResult == true else { return }
                // Core Animation can skip an initial drawable. Request a fresh
                // protected publication, never another read of this old source.
                self.needsNewPublication = true
            }, record: record)
        return true
    }

    func move(to point: CGPoint, hotSpot: CGPoint = .zero) {
        guard point.x.isFinite, point.y.isFinite,
              hotSpot.x.isFinite, hotSpot.y.isFinite else { return }
        pointer = point
        self.hotSpot = hotSpot
        updatePosition()
        updateOutput()
        updateClock()
    }

    func setVisible(_ visible: Bool) {
        let wasVisible = self.visible
        self.visible = visible
        if wasVisible && !visible { resetContent() }
        else { presenter?.setDrawableAllowed(visible) }
        if visible, presenter != nil { panel.orderFrontRegardless() }
        else {
            panel.orderOut(nil)
            flushLatches()
        }
        updateOutput()
        updateClock()
    }

    func setSubmissionAllowed(_ allowed: Bool) {
        submissionAllowed = allowed
        presenter?.setSubmissionAllowed(allowed)
        updateClock()
    }

    func fenceSubmission(_ completion: @escaping @MainActor @Sendable () -> Void) {
        var queues = Array(retiredPresenters.values)
        if let presenter { queues.append(presenter) }
        guard !queues.isEmpty else { flushLatches(); completion(); return }
        let fence = SubmissionFence(remaining: queues.count)
        for queue in queues {
            queue.fenceSubmission { [weak self] in
                fence.remaining -= 1
                if fence.remaining == 0 {
                    self?.flushLatches()
                    completion()
                }
            }
        }
    }

    /// A role or visibility boundary starts with an empty drawable pool. The
    /// old worker retains its own layer until its submission fence completes.
    func resetContent() {
        publicationGeneration &+= 1
        publication = nil
        needsNewPublication = false
        panel.orderOut(nil)
        displayClock.unregister(self)
        if let presenter {
            presenter.setSubmissionAllowed(false)
            presenter.setDrawableAllowed(false)
            presenter.cancelPending()
            let key = ObjectIdentifier(presenter)
            retiredPresenters[key] = presenter
            presenter.fenceSubmission { [weak self] in
                self?.flushLatches()
                self?.retiredPresenters.removeValue(forKey: key)
            }
        }
        presenter = nil
        presenterRenderer = nil
        needsDisplayRetry = false
        let layer = CAMetalLayer()
        Self.configure(layer)
        layer.frame = view.bounds
        metalLayer = layer
        view.layer = layer
        flushLatches()
    }

    /// Submitted records stay owned by their actual drawable callbacks even
    /// when the transport detaches and this panel is reused by a new session.
    func close() {
        visible = false
        resetContent()
        submissionAllowed = true
        panel.close()
    }

    private final class SubmissionFence {
        var remaining: Int
        init(remaining: Int) { self.remaining = remaining }
    }

    private static func configure(_ layer: CAMetalLayer) {
        layer.pixelFormat = .bgra8Unorm
        layer.isOpaque = false
        layer.framebufferOnly = false
        layer.maximumDrawableCount = 3
        layer.allowsNextDrawableTimeout = true
    }

    func displayClockFired(_ link: CADisplayLink) {
        updateOutput()
        flushLatches()
        if needsDisplayRetry, visible, submissionAllowed {
            needsDisplayRetry = false
            presenter?.resumeAfterDisplayTick()
        }
        if needsNewPublication, visible, submissionAllowed, let publication {
            needsNewPublication = false
            onNeedsNewPublication?(publication.surface, publication.presentationID)
        }
    }

    func windowDidChangeScreen(_ notification: Notification) {
        updateOutput()
        updateClock()
    }

    func windowDidChangeBackingProperties(_ notification: Notification) {
        updateOutput()
        updateClock()
    }

    private func updatePosition() {
        panel.setFrameOrigin(Self.origin(kind: kind, pointer: pointer,
            logicalSize: logicalSize, hotSpot: hotSpot))
    }

    private func updateOutput() {
        // A small moving overlay can span displays. Its drawable callback
        // proves a time, but does not identify the actual physical output.
        output.update(outputID: 0, refreshNanoseconds: 0)
    }

    private func updateClock() {
        if visible, submissionAllowed, presenter != nil {
            displayClock.register(self, screen: panel.screen)
        } else { displayClock.unregister(self) }
    }

    private func flushLatches() {
        let latches = pendingLatches
        pendingLatches.removeAll(keepingCapacity: true)
        for latch in latches { latch() }
    }

    static func origin(kind: Kind, pointer: CGPoint, logicalSize: CGSize, hotSpot: CGPoint) -> CGPoint {
        switch kind {
        case .cursor:
            return CGPoint(x: pointer.x - hotSpot.x,
                y: pointer.y - logicalSize.height + hotSpot.y)
        case .drag:
            return CGPoint(x: pointer.x + 8, y: pointer.y - logicalSize.height - 8)
        }
    }

    struct DrawingGeometry {
        let scene: Windowing.SceneSnapshot
        let drawableSize: CGSize
    }

    static func drawingGeometry(frame: Windowing.Frame, surface: UInt32,
        logicalSize: CGSize, backingScale: CGFloat) -> DrawingGeometry? {
        let pixelWidth = (logicalSize.width * backingScale).rounded(.up)
        let pixelHeight = (logicalSize.height * backingScale).rounded(.up)
        guard surface != 0, backingScale.isFinite, backingScale > 0,
              pixelWidth.isFinite, pixelHeight.isFinite,
              pixelWidth >= 1, pixelHeight >= 1, pixelWidth <= 32_768, pixelHeight <= 32_768,
              logicalSize.width.isFinite, logicalSize.height.isFinite,
              logicalSize.width > 0, logicalSize.height > 0,
              logicalSize.width <= 32_768, logicalSize.height <= 32_768,
              backingScale <= 32_768 else { return nil }
        let width = Int(pixelWidth), height = Int(pixelHeight)
        let source = frame.fullViewportBufferPixelRect
        let layer = Windowing.SceneLayer(surface: surface, resourceID: frame.resourceID,
            width: frame.width, height: frame.height, bytesPerRow: frame.bytesPerRow,
            format: frame.format,
            destination: .init(x: 0, y: 0, width: Double(width), height: Double(height)),
            sourcePixels: .init(x: Double(source.minX), y: Double(source.minY),
                width: Double(source.width), height: Double(source.height)),
            clip: .init(x: 0, y: 0, width: Double(width), height: Double(height)),
            alpha: 1, opaque: frame.format == .bgrx8888, transform: .normal)
        let scene = Windowing.SceneSnapshot(surface: surface, presentationID: frame.presentationID,
            width: width, height: height, scale: max(1, Int(backingScale.rounded())),
            windowGeometry: .init(x: 0, y: 0,
                width: max(1, Int(logicalSize.width.rounded(.up))),
                height: max(1, Int(logicalSize.height.rounded(.up)))),
            layers: [layer], damage: [.init(x: 0, y: 0, width: width, height: height)])
        return DrawingGeometry(scene: scene, drawableSize: CGSize(width: width, height: height))
    }
}
