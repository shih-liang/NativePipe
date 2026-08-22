import AppKit
import ImageIO
import IOSurface
import NativePipeProtocol
import UniformTypeIdentifiers

/// Where a committed frame's pixels come from.
///
/// Local window frames name a compositor-owned Venus image. The frame source
/// exposes that image as an MTLTexture-shaped object without making this module
/// depend on the virtual GPU implementation. Remote frames still arrive as an
/// IOSurface from VideoToolbox.
@MainActor
public protocol FrameSource: AnyObject {
    func surface(forResource resourceID: UInt32) -> IOSurfaceRef?
    func metalTexture(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> AnyObject?
}

extension FrameSource {
    public func metalTexture(
        forResource resourceID: UInt32,
        width: Int, height: Int, bytesPerRow: Int, format: UInt32
    ) -> AnyObject? { nil }
}

/// Applies guest window events to `NSWindow`s, and sends host decisions back.
///
/// The guest owns the Wayland scene graph, scale/viewport resolution and frame
/// scheduling. This bridge receives one already-composited Venus image per xdg
/// window and asks the NSWindow to copy it into its private display IOSurface.
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
        private var displayedImage: CGImage?

        init() {
            panel = NSPanel(
                contentRect: .zero,
                styleMask: [.borderless, .nonactivatingPanel],
                backing: .buffered,
                defer: false)
            view.wantsLayer = true
            view.layer?.contentsGravity = .resize
            view.layer?.isOpaque = false
            panel.contentView = view
            panel.backgroundColor = .clear
            panel.isOpaque = false
            panel.hasShadow = false
            panel.ignoresMouseEvents = true
            panel.level = .popUpMenu
            panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        }

        func display(_ surface: IOSurfaceRef, frame: Windowing.Frame) {
            let scale = CGFloat(max(frame.scale, 1))
            let size = NSSize(
                width: CGFloat(frame.width) / scale,
                height: CGFloat(frame.height) / scale)

            // CALayer can present the window-sized IOSurfaces directly, but it
            // treats a small standalone IOSurface as opaque on some systems.
            // Wayland ARGB8888 is explicitly premultiplied alpha, so construct
            // a CGImage with that exact bitmap description for the drag icon.
            // Icons are tiny and update rarely; this copy is intentionally not
            // part of the zero-copy application-window path.
            guard IOSurfaceLock(surface, [.readOnly], nil) == kIOReturnSuccess else { return }
            defer { IOSurfaceUnlock(surface, [.readOnly], nil) }
            let base = IOSurfaceGetBaseAddress(surface)
            let byteCount = frame.bytesPerRow * frame.height
            let bytes = Data(bytes: base, count: byteCount)

            if WindowBridge.trace, frame.format == .bgra8888 {
                var minimum: UInt8 = 255
                var maximum: UInt8 = 0
                var transparent = 0
                var translucent = 0
                bytes.withUnsafeBytes { raw in
                    guard let data = raw.bindMemory(to: UInt8.self).baseAddress else { return }
                    for y in 0..<frame.height {
                        let row = data + y * frame.bytesPerRow
                        for x in 0..<frame.width {
                            let alpha = row[x * 4 + 3]
                            minimum = min(minimum, alpha)
                            maximum = max(maximum, alpha)
                            if alpha == 0 { transparent += 1 }
                            else if alpha != 255 { translucent += 1 }
                        }
                    }
                }
                WindowBridge.note(
                    "drag icon alpha min=\(minimum) max=\(maximum) " +
                    "transparent=\(transparent) translucent=\(translucent) " +
                    "pixels=\(frame.width * frame.height)")
            }
            guard let provider = CGDataProvider(data: bytes as CFData) else { return }

            let alpha: CGImageAlphaInfo
            let byteOrder: CGBitmapInfo
            switch frame.format {
            case .bgra8888:
                alpha = .premultipliedFirst
                byteOrder = .byteOrder32Little
            case .bgrx8888:
                alpha = .noneSkipFirst
                byteOrder = .byteOrder32Little
            case .rgba8888:
                alpha = .premultipliedLast
                byteOrder = .byteOrder32Big
            }
            let bitmapInfo = byteOrder.union(CGBitmapInfo(rawValue: alpha.rawValue))
            guard let image = CGImage(
                width: frame.width,
                height: frame.height,
                bitsPerComponent: 8,
                bitsPerPixel: 32,
                bytesPerRow: frame.bytesPerRow,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: bitmapInfo,
                provider: provider,
                decode: nil,
                shouldInterpolate: true,
                intent: .defaultIntent)
            else { return }
            displayedImage = image

            if let path = ProcessInfo.processInfo.environment["NATIVEPIPE_DRAG_ICON_DUMP"] {
                let url = URL(fileURLWithPath: path) as CFURL
                if let destination = CGImageDestinationCreateWithURL(
                    url, UTType.png.identifier as CFString, 1, nil
                ) {
                    CGImageDestinationAddImage(destination, image, nil)
                    CGImageDestinationFinalize(destination)
                }
            }
            panel.setContentSize(size)
            view.layer?.contentsScale = scale
            view.layer?.contents = image
            moveToPointer()
            panel.orderFrontRegardless()
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
            view.layer?.contents = nil
            displayedImage = nil
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
    private var pointerCursor = NSCursor.arrow

    private var windows: [UInt32: NativeWindow] = [:]
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

    let clipboard = ClipboardBridge()

    public init(frameSource: FrameSource?) {
        self.frameSource = frameSource
        clipboard.output = { [weak self] command in self?.send(command) }
        clipboard.start()
    }

    public var windowCount: Int { windows.count }

    func window(_ id: UInt32) -> NativeWindow? { windows[id] }

    func currentPointerCursor() -> NSCursor { pointerCursor }

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
        if case .committed(let surface, let frame) = event {
            Self.note(
                "commit surface=\(surface) res=\(frame.resourceID) \(frame.width)x\(frame.height) source=\(frame.source)")
        } else {
            Self.note("\(event)")
        }

        switch event {
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
            if dragIconSurface == surface {
                dragIconSurface = nil
                dragIcon.hide()
            }
            if let windowID = surfaceToWindow.removeValue(forKey: surface) {
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

        case .popupCreated(let window, let surface, let parent, let x, let y, _, _):
            // Like a toplevel, the NSWindow waits for the first frame; a menu
            // that flashes empty before it draws is worse than one that appears
            // a frame later.
            let native = NativeWindow(
                windowID: window, surfaceID: surface, bridge: self,
                popup: NativeWindow.Popup(parent: parent, origin: CGPoint(x: x, y: y)))
            windows[window] = native
            surfaceToWindow[surface] = window

        case .popupDestroyed(let window):
            if let native = windows.removeValue(forKey: window) {
                surfaceToWindow.removeValue(forKey: native.surfaceID)
                native.close()
            }

        case .toplevelDestroyed(let window):
            if let native = windows.removeValue(forKey: window) {
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
                guard let ioSurface = frameSource?.surface(forResource: frame.resourceID) else {
                    retainDeferred(frame, for: surface)
                    return
                }
                dragIcon.display(ioSurface, frame: frame)
                completeCopiedPresentation(
                    surface: surface, presentationID: frame.presentationID)
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

        case .decorationModeChanged(let window, let serverSide):
            windows[window]?.setServerDecorated(serverSide)

        case .parentChanged(let window, let parent):
            windows[window]?.setParent(parent.flatMap { windows[$0] })

        case .sizeConstraintsChanged(let window, let minimum, let maximum):
            windows[window]?.setConstraints(minimum: minimum, maximum: maximum)

        case .committed(let surface, let frame):
            if surface == dragIconSurface {
                guard let ioSurface = frameSource?.surface(forResource: frame.resourceID) else {
                    retainDeferred(frame, for: surface)
                    Self.note("drag icon commit deferred: no output IOSurface for \(frame.resourceID)")
                    return
                }
                pendingFrames.removeValue(forKey: surface)
                dragIcon.display(ioSurface, frame: frame)
                completeCopiedPresentation(
                    surface: surface, presentationID: frame.presentationID)
                return
            }
            guard let windowID = surfaceToWindow[surface] else {
                retainUnroled(frame, for: surface)
                Self.note("commit retained: surface \(surface) has no role yet")
                return
            }
            presentCommitted(surface: surface, windowID: windowID, frame: frame)

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
            guard let nsWindow = windows[window]?.window,
                  nsWindow.styleMask.contains(.fullScreen) != enabled
            else { return }
            nsWindow.toggleFullScreen(nil)

        case .maximizeRequested(let window, let enabled):
            guard let nsWindow = windows[window]?.window, nsWindow.isZoomed != enabled else { return }
            nsWindow.zoom(nil)

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
        guard !pendingFrames.isEmpty else { return }
        let snapshot = pendingFrames
        for (surface, frame) in snapshot {
            apply(.committed(surface: surface, frame: frame))
        }
    }

    /// Replace a frame the host has not installed only after completing the
    /// superseded presentation id. Once a commit has crossed the guest/host
    /// boundary, that id owns a guest output-ring slot even if its IOSurface is
    /// not visible in the host resource table yet.
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
        // virgl_hw.h: BGRA8_UNORM=1, RGBA8_UNORM=67. Window scene output is
        // always BGRA, but retain the format mapping for legacy/unroled frames.
        let virglFormat: UInt32 = frame.format == .rgba8888 ? 67 : 1
        if frame.source != .encoded,
           let texture = frameSource?.metalTexture(
               forResource: frame.resourceID,
               width: frame.width, height: frame.height,
               bytesPerRow: frame.bytesPerRow, format: virglFormat)
        {
            pendingFrames.removeValue(forKey: surface)
            native.present(
                frame: frame, metalTexture: texture,
                copied: { [weak self] success in
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
            if Self.frameTrace {
                Self.note("blit queued window=\(windowID) res=\(frame.resourceID)")
            }
            native.traceLayerGeometry()
            injectTestInput(windowID)
            scheduleResizeProbe(native)
            return
        }

        guard let ioSurface = frameSource?.surface(forResource: frame.resourceID) else {
            retainDeferred(frame, for: surface)
            Self.note("commit deferred: no Metal texture or IOSurface for resource \(frame.resourceID)")
            return
        }
        pendingFrames.removeValue(forKey: surface)
        native.present(frame: frame, surface: ioSurface)
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
        for (_, window) in windows { window.close() }
        windows.removeAll()
        surfaceToWindow.removeAll()
        knownSurfaces.removeAll()
    }
}
