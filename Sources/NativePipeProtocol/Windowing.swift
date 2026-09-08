import Foundation

/// The window protocol between the guest's Wayland translator and the host.
///
/// This is deliberately *not* Wayland. The guest side runs the Wayland protocol
/// state machine — surface roles, commit atomicity, buffer release timing — and
/// forwards only what macOS needs to put a window on screen. The guest resolves
/// scene graph, stacking, scale, viewport and synchronized-commit semantics
/// into an immutable layer snapshot. The host samples the already-existing
/// Metal textures and performs the one composition needed for the drawable.
///
/// The mapping is one-to-one and stays that way:
///
///     wl_surface   #17  ->  NativeSurface #17
///     xdg_toplevel #23  ->  NativeWindow  #23  ->  NSWindow *
public enum Windowing {}

// MARK: - Guest -> host

extension Windowing {
	public struct Display: Codable, Sendable, Equatable {
		public var id: UInt32
		public var name: String
		public var x: Int
		public var y: Int
		public var width: Int
		public var height: Int
		public var pixelWidth: Int
		public var pixelHeight: Int
		public var physicalWidthMM: Int
		public var physicalHeightMM: Int
		public var scale: Int
		public var refreshMilliHz: Int

		public init(
			id: UInt32, name: String, x: Int, y: Int,
			width: Int, height: Int, pixelWidth: Int, pixelHeight: Int,
			physicalWidthMM: Int, physicalHeightMM: Int,
			scale: Int, refreshMilliHz: Int
		) {
			self.id = id; self.name = name; self.x = x; self.y = y
			self.width = width; self.height = height
			self.pixelWidth = pixelWidth; self.pixelHeight = pixelHeight
			self.physicalWidthMM = physicalWidthMM
			self.physicalHeightMM = physicalHeightMM
			self.scale = scale; self.refreshMilliHz = refreshMilliHz
		}
	}

    /// Things the guest's translator reports upward.
    public enum GuestEvent: Sendable {
        /// The compositor sends this before replaying its authoritative state
        /// on every new transport connection. A connected vsock alone is not
        /// evidence that the guest event loop owns and can write the channel.
        case channelReady(sessionID: UInt32, protocolVersion: UInt32)

        case surfaceCreated(surface: UInt32)
        case surfaceDestroyed(surface: UInt32)
        /// A null-buffer commit unmaps the role without destroying it. The host
        /// closes only the physical NSWindow and keeps the role for a later
        /// initial-configure/remap sequence.
        case surfaceUnmapped(surface: UInt32)

        /// A surface took the toplevel role. No `NSWindow` is created yet: one
        /// appears on the first commit that carries a frame, because a window
        /// shown before it has content flashes empty.
        case toplevelCreated(window: UInt32, surface: UInt32)
        /// True only when the compositor can identify and terminate the
        /// Wayland client that owns this toplevel.  Xwayland-satellite owns the
        /// Wayland connection for all X11 windows, so killing that peer would
        /// incorrectly terminate every X11 application in the session.
        case forceQuitCapabilityChanged(window: UInt32, supported: Bool)
        case toplevelDestroyed(window: UInt32)

        /// A surface took the popup role: a menu, dropdown or tooltip anchored to
        /// another window. Position is already resolved against the positioner
        /// the client supplied, relative to the parent's xdg window geometry.
        case popupCreated(
            window: UInt32, surface: UInt32, parent: UInt32,
            x: Int, y: Int, width: Int, height: Int)
        case popupPlacementRequested(PopupPlacement)
        case popupRepositioned(
            window: UInt32, x: Int, y: Int, width: Int, height: Int)
        case popupDestroyed(window: UInt32)

        /// Legacy compatibility events. Current guests composite subsurfaces
        /// before publishing a frame, so the host ignores these cases.
        case subsurfaceCreated(surface: UInt32, parent: UInt32, x: Int, y: Int)
        case subsurfaceMoved(surface: UInt32, x: Int, y: Int)
        case subsurfaceDestroyed(surface: UInt32)

        /// The surface supplied to wl_data_device.start_drag. It is a transient,
        /// pointer-following image rather than a window or subsurface. Nil ends
        /// the overlay when the drag completes or is cancelled.
        case dragIconChanged(surface: UInt32?)

        /// A wl_pointer.set_cursor surface and hotspot, or nil for the default
        /// pointer. Semantic cursor-shape-v1 cursors use the separate case.
        case cursorChanged(surface: UInt32?, hotspotX: Int, hotspotY: Int)
        case cursorShapeChanged(shape: CursorShape)

        case titleChanged(window: UInt32, title: String)
        case appIDChanged(window: UInt32, appID: String)
        /// Whether AppKit owns the window chrome. If false, the client draws
        /// CSD over a transparent full-size AppKit title bar.
        case decorationModeChanged(window: UInt32, serverSide: Bool)
        case parentChanged(window: UInt32, parent: UInt32?)
        case sizeConstraintsChanged(window: UInt32, minimum: Size?, maximum: Size?)

        /// The client acknowledged a configure and attached matching content.
        case committed(surface: UInt32, frame: Frame)

        /// One atomic xdg-window scene. This case is carried by the bounded
        /// binary NPSN wire message; `layers` are ordered from
        /// back to front and name existing virtio-gpu resources.
        case sceneCommitted(scene: SceneSnapshot)

        /// A committed update carried frame/FIFO state but no new buffer. The
        /// host completes it on the owning window's next display refresh.
        case frameCallbackRequested(surface: UInt32, presentationID: UInt32)

        /// `xdg_toplevel.move` / `.resize` — the client asking the host to run an
        /// interactive drag. Client-side decorations report their title bar drags
        /// this way, so the host never has to guess at draggable regions.
        case interactiveMoveRequested(window: UInt32, serial: UInt32)
        case interactiveResizeRequested(window: UInt32, edges: ResizeEdge, serial: UInt32)

        case fullscreenRequested(window: UInt32, enabled: Bool)
        case maximizeRequested(window: UInt32, enabled: Bool)
        case minimizeRequested(window: UInt32)

        // MARK: Clipboard
        //
        // The selection is announced as a type list and fetched separately,
        // because that is the shape both sides already have: wl_data_source
        // advertises MIME types and hands over bytes only on request, and
        // NSPasteboard is likewise a set of declared types. Copying the bytes
        // eagerly at announce time would move data nobody asked for — a large
        // image copied inside the guest would cross the channel even if the user
        // never pasted it on the Mac.

        /// A guest client took the selection, offering these MIME types. An
        /// empty list means the selection was cleared.
        case selectionOffered(mimeTypes: [String])
        /// The bytes for a `selectionRequest`. Nil data means the source could
        /// not supply that type; the host must not wait for a retry.
        case selectionData(token: UInt32, mimeType: String, data: Data?)
        /// A guest client is pasting and wants the macOS pasteboard's contents.
        case hostSelectionRequest(token: UInt32, mimeType: String)

        // MARK: Text input
        //
        // zwp_text_input_v3 exists so a client can say "I have a text field
        // here" without also claiming to know how text gets composed. That is
        // exactly the split NativePipe needs: composition is macOS's, and the
        // guest only ever learns the result. Raw keys still flow for everything
        // that is not text — arrows, Return, Escape — because those are editing
        // commands, not characters.

        /// A client enabled or disabled text input on this window. Enabling is
        /// what makes the macOS input context active for it.
        case textInputEnabled(window: UInt32, enabled: Bool)
        /// Where the caret is, in surface-local coordinates. The IME candidate
        /// window is positioned against this.
        case textInputCursorRect(window: UInt32, x: Int, y: Int, width: Int, height: Int)
        /// Text around the caret, which is what lets an IME do reconversion and
        /// context-sensitive conversion rather than composing blind.
        case textInputSurroundingText(window: UInt32, text: String, cursor: Int, anchor: Int)
    }

    public struct FloatRect: Codable, Sendable, Equatable {
        public var x: Double
        public var y: Double
        public var width: Double
        public var height: Double

        public init(x: Double, y: Double, width: Double, height: Double) {
            self.x = x
            self.y = y
            self.width = width
            self.height = height
        }
    }

    public struct PopupPlacement: Codable, Sendable, Equatable {
        public var window: UInt32
        public var parent: UInt32
        public var x: Int
        public var y: Int
        public var flippedX: Int
        public var flippedY: Int
        public var width: Int
        public var height: Int
        public var adjustment: UInt32
        public var token: UInt32
        public var reactive: Bool

        public init(
            window: UInt32, parent: UInt32,
            x: Int, y: Int, flippedX: Int, flippedY: Int,
            width: Int, height: Int, adjustment: UInt32,
            token: UInt32, reactive: Bool
        ) {
            self.window = window
            self.parent = parent
            self.x = x
            self.y = y
            self.flippedX = flippedX
            self.flippedY = flippedY
            self.width = width
            self.height = height
            self.adjustment = adjustment
            self.token = token
            self.reactive = reactive
        }
    }

    /// The buffer transform already applied while resolving a scene layer.
    /// Raw values intentionally match Wayland's `wl_output_transform` values.
    public enum BufferTransform: UInt32, Codable, Sendable, Equatable {
        case normal = 0
        case rotate90 = 1
        case rotate180 = 2
        case rotate270 = 3
        case flipped = 4
        case flipped90 = 5
        case flipped180 = 6
        case flipped270 = 7
    }

    /// One source texture in a host-composited xdg-window scene.
    public struct SceneLayer: Codable, Sendable, Equatable {
        public var surface: UInt32
        public var resourceID: UInt32
        public var width: Int
        public var height: Int
        public var bytesPerRow: Int
        public var format: PixelFormat
        /// Destination in output pixels, relative to window geometry.
        public var destination: FloatRect
        /// Sample rectangle in source-buffer pixels.
        public var sourcePixels: FloatRect
        /// Destination-space scissor in output pixels.
        public var clip: FloatRect
        public var alpha: Float
        public var opaque: Bool
        public var transform: BufferTransform

        public init(
            surface: UInt32, resourceID: UInt32,
            width: Int, height: Int, bytesPerRow: Int, format: PixelFormat,
            destination: FloatRect, sourcePixels: FloatRect, clip: FloatRect,
            alpha: Float = 1, opaque: Bool = false,
            transform: BufferTransform = .normal
        ) {
            self.surface = surface
            self.resourceID = resourceID
            self.width = width
            self.height = height
            self.bytesPerRow = bytesPerRow
            self.format = format
            self.destination = destination
            self.sourcePixels = sourcePixels
            self.clip = clip
            self.alpha = alpha
            self.opaque = opaque
            self.transform = transform
        }
    }

    /// Immutable scene state associated with one presentation id.
    public struct SceneSnapshot: Codable, Sendable, Equatable {
        public var surface: UInt32
        public var presentationID: UInt32
        public var width: Int
        public var height: Int
        public var scale: Int
        public var windowGeometry: Rect
		/// Host-private serial of the most recent AppKit configure acknowledged by
		/// the Wayland commit that produced this scene. It binds actual client
		/// geometry to the configure generation without changing xdg-shell serials.
		public var configureSerial: UInt32
        public var layers: [SceneLayer]
        /// Output-pixel regions whose composited result changed. The host keeps
        /// a short damage history for each drawable slot; an empty list means a
        /// latch-only scene.
        public var damage: [Rect]

        public init(
            surface: UInt32, presentationID: UInt32,
            width: Int, height: Int, scale: Int,
			windowGeometry: Rect, configureSerial: UInt32 = 0,
			layers: [SceneLayer], damage: [Rect] = []
        ) {
            self.surface = surface
            self.presentationID = presentationID
            self.width = width
            self.height = height
            self.scale = scale
            self.windowGeometry = windowGeometry
			self.configureSerial = configureSerial
            self.layers = layers
            self.damage = damage
        }
    }

    /// Values assigned by wp_cursor_shape_device_v1.set_shape.
    public enum CursorShape: UInt32, Codable, Sendable, Equatable {
        case defaultShape = 1, contextMenu, help, pointer, progress, wait
        case cell, crosshair, text, verticalText, alias, copy, move
        case noDrop, notAllowed, grab, grabbing
        case eResize, nResize, neResize, nwResize
        case sResize, seResize, swResize, wResize
        case ewResize, nsResize, neswResize, nwseResize
        case colResize, rowResize, allScroll, zoomIn, zoomOut
    }

    /// What a committed frame consists of.
    ///
    /// The pixels are not in here. `resourceID` names a virtio-gpu blob that is
    /// already host memory, so the host looks the resource up rather than
    /// receiving anything.
    public struct Frame: Codable, Sendable {
        public var resourceID: UInt32
        public var width: Int
        public var height: Int
        public var bytesPerRow: Int
        public var format: PixelFormat
        /// Pixel density of this resource's logical surface.
        public var scale: Int
        /// The visible xdg-shell window inside the wl_surface, in logical
        /// surface coordinates. CSD clients commonly leave transparent shadow
        /// margins outside this rectangle; those margins are not part of the
        /// native window size or its input coordinate origin.
        public var windowGeometry: Rect?
        /// Damage in surface-local pixels. Empty means the whole surface.
        public var damage: [Rect]
        /// How this frame was produced. CPU is a guest wl_shm upload into one
        /// Vulkan texture. GPU already lives in MoltenVK on the host — the
        /// resource id is only a name, not a copy.
        /// Encoded means a remote H.264 (etc.) stream keyed by resourceID.
        public var source: FrameSourceKind
        /// Codec id for `.encoded` frames (`h264`, …). Nil for cpu/gpu.
        public var codec: String?
        /// Bumped when the remote encoder is reset so the host rebuilds its
        /// decompression session.
        public var bitstreamEpoch: UInt16
        /// Nonzero identity used to release presentation source references only
        /// after the host has consumed them.
        public var presentationID: UInt32
        /// Legacy per-surface viewporter metadata. Current window frames have
        /// already consumed this in the guest compositor; cursor/drag surfaces
        /// can still carry it because they are not part of a window scene.
        public var viewportSource: FloatRect?
        public var viewportDestination: Size?

        public init(
            resourceID: UInt32, width: Int, height: Int, bytesPerRow: Int,
            format: PixelFormat, scale: Int = 1, windowGeometry: Rect? = nil,
            damage: [Rect] = [], source: FrameSourceKind = .cpu,
            codec: String? = nil, bitstreamEpoch: UInt16 = 0,
            presentationID: UInt32 = 0,
            viewportSource: FloatRect? = nil,
            viewportDestination: Size? = nil
        ) {
            self.resourceID = resourceID
            self.width = width
            self.height = height
            self.bytesPerRow = bytesPerRow
            self.format = format
            self.scale = scale
            self.windowGeometry = windowGeometry
            self.damage = damage
            self.source = source
            self.codec = codec
            self.bitstreamEpoch = bitstreamEpoch
            self.presentationID = presentationID
            self.viewportSource = viewportSource
            self.viewportDestination = viewportDestination
        }

        enum CodingKeys: String, CodingKey {
            case resourceID, width, height, bytesPerRow, format, scale
            case windowGeometry, damage, source, codec, bitstreamEpoch
            case presentationID, viewportSource, viewportDestination
        }

        public init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            resourceID = try c.decode(UInt32.self, forKey: .resourceID)
            width = try c.decode(Int.self, forKey: .width)
            height = try c.decode(Int.self, forKey: .height)
            bytesPerRow = try c.decode(Int.self, forKey: .bytesPerRow)
            format = try c.decode(PixelFormat.self, forKey: .format)
            scale = try c.decodeIfPresent(Int.self, forKey: .scale) ?? 1
            windowGeometry = try c.decodeIfPresent(Rect.self, forKey: .windowGeometry)
            damage = try c.decodeIfPresent([Rect].self, forKey: .damage) ?? []
            source = try c.decodeIfPresent(FrameSourceKind.self, forKey: .source) ?? .cpu
            codec = try c.decodeIfPresent(String.self, forKey: .codec)
            bitstreamEpoch = try c.decodeIfPresent(UInt16.self, forKey: .bitstreamEpoch) ?? 0
            presentationID = try c.decodeIfPresent(UInt32.self, forKey: .presentationID) ?? 0
            viewportSource = try c.decodeIfPresent(FloatRect.self, forKey: .viewportSource)
            viewportDestination = try c.decodeIfPresent(Size.self, forKey: .viewportDestination)
        }

        public func encode(to encoder: Encoder) throws {
            var c = encoder.container(keyedBy: CodingKeys.self)
            try c.encode(resourceID, forKey: .resourceID)
            try c.encode(width, forKey: .width)
            try c.encode(height, forKey: .height)
            try c.encode(bytesPerRow, forKey: .bytesPerRow)
            try c.encode(format, forKey: .format)
            try c.encode(scale, forKey: .scale)
            try c.encodeIfPresent(windowGeometry, forKey: .windowGeometry)
            try c.encode(damage, forKey: .damage)
            if source != .cpu { try c.encode(source, forKey: .source) }
            try c.encodeIfPresent(codec, forKey: .codec)
            if bitstreamEpoch != 0 { try c.encode(bitstreamEpoch, forKey: .bitstreamEpoch) }
            if presentationID != 0 { try c.encode(presentationID, forKey: .presentationID) }
            try c.encodeIfPresent(viewportSource, forKey: .viewportSource)
            try c.encodeIfPresent(viewportDestination, forKey: .viewportDestination)
        }
    }

    /// Guest compositor's classification of a committed buffer.
    public enum FrameSourceKind: String, Codable, Sendable {
        /// `wl_shm`: compositor copied into a host IOSurface.
        case cpu
        /// Venus / linux-dmabuf: the virtio-gpu resource *is* the host
        /// MoltenVK image. No second allocation.
        case gpu
        /// Remote display: pixels arrive as a compressed bitstream on the
        /// media port; `resourceID` names the stream (usually the surface id).
        case encoded
    }

    public enum PixelFormat: String, Codable, Sendable, Equatable {
        /// `WL_SHM_FORMAT_ARGB8888` little-endian: premultiplied BGRA bytes.
        case bgra8888
        /// `WL_SHM_FORMAT_XRGB8888` little-endian: the high byte is padding,
        /// not alpha. Keeping it distinct matters for translucent overlays.
        case bgrx8888
        case rgba8888
    }

    public struct Size: Codable, Sendable, Equatable {
        public var width: Int
        public var height: Int
        public init(width: Int, height: Int) {
            self.width = width
            self.height = height
        }
    }

    public struct Rect: Codable, Sendable, Equatable {
        public var x: Int
        public var y: Int
        public var width: Int
        public var height: Int
        public init(x: Int, y: Int, width: Int, height: Int) {
            self.x = x
            self.y = y
            self.width = width
            self.height = height
        }
    }

    public struct ResizeEdge: OptionSet, Codable, Sendable {
        public let rawValue: UInt32
        public init(rawValue: UInt32) { self.rawValue = rawValue }

        public init(from decoder: Decoder) throws {
            rawValue = try decoder.singleValueContainer().decode(UInt32.self)
        }

        public func encode(to encoder: Encoder) throws {
            var container = encoder.singleValueContainer()
            try container.encode(rawValue)
        }

        public static let top = ResizeEdge(rawValue: 1 << 0)
        public static let bottom = ResizeEdge(rawValue: 1 << 1)
        public static let left = ResizeEdge(rawValue: 1 << 2)
        public static let right = ResizeEdge(rawValue: 1 << 3)
    }
}

// MARK: - Host -> guest

extension Windowing {
    /// Instructions the host sends down. Geometry, focus and lifetime are macOS
    /// decisions; the guest applies them to the Wayland objects.
    public enum HostCommand: Sendable {
        /// The window changed size or state. `size` is in logical window-
        /// geometry coordinates (AppKit points), never backing pixels. The
        /// client's wl_surface buffer scale determines pixel density separately.
        case configure(window: UInt32, size: Size, states: [ToplevelState], serial: UInt32)
        /// User clicked the close button. This is a request — the client decides.
        case close(window: UInt32)
        /// The user explicitly confirmed Force Quit. The guest compositor kills
        /// only the process that owns this Wayland connection.
        case forceQuit(window: UInt32)

        /// Dismiss a popup: the click went elsewhere, or the parent lost focus.
        /// Like close, this is a request; the client tears the popup down.
        case dismissPopup(window: UInt32)
        /// Host-resolved popup geometry in parent-local logical coordinates.
        /// The guest turns this into xdg_popup.configure; AppKit moves only
        /// after the client acknowledges and commits that configure.
        case configurePopup(
            window: UInt32, x: Int, y: Int, width: Int, height: Int,
            token: UInt32)
        /// Backing scale of the screen the window is on, which changes when the
        /// user drags it between displays.
        case scaleChanged(window: UInt32, scale: Int)
		/// Complete current physical-display topology. It changes only when macOS
		/// screen parameters change, not per frame or per window.
		case outputsChanged(displays: [Display])
		/// The wl_output currently containing this xdg window. Nil leaves all
		/// outputs, for example while AppKit is moving it between screens.
        case windowOutputChanged(window: UInt32, outputID: UInt32?)
        /// Host input policy. Layout is an XKB layout name; rate zero disables
        /// repeat. Existing wl_keyboard resources receive the new keymap.
        case inputPreferences(layout: String, repeatRate: Int, repeatDelay: Int)

        case keyboardFocus(window: UInt32?)
        case key(window: UInt32, keycode: UInt32, pressed: Bool, modifiers: Modifiers)
        case pointerEntered(window: UInt32, x: Double, y: Double)
        case pointerMoved(window: UInt32, x: Double, y: Double)
        case pointerLeft(window: UInt32)
        case pointerButton(window: UInt32, button: PointerButton, pressed: Bool)
        /// Precise displacement is in logical points. A precise (0, 0) record
        /// explicitly ends the gesture; it is not an idle motion sample.
        case pointerScroll(window: UInt32, dx: Double, dy: Double, isPrecise: Bool)

        /// The frame reached the host display clock. This completes Wayland
        /// frame callbacks and FIFO barriers, but does not make the currently
        /// displayed IOSurface writable again.
        case framePresented(surface: UInt32, presentationID: UInt32)
        /// A later CALayer contents transaction no longer references this
        /// frame, so its guest output-ring slot can be reused safely.
        case frameReleased(surface: UInt32, presentationID: UInt32)

        /// Ask the guest compositor to republish its current immutable scene.
        /// The resulting presentation owns every source buffer until the host
        /// has copied the composed drawable for an explicit Computer Use
        /// capture. This avoids WindowServer/ScreenCaptureKit entirely.
        case captureFrame(surface: UInt32)

        // MARK: Clipboard — the mirror image of the guest's three events.

        /// The host is pasting into macOS and needs the guest selection's bytes.
        case selectionRequest(token: UInt32, mimeType: String)
        /// The macOS pasteboard changed and now offers these MIME types. An
        /// empty list clears the selection inside the guest.
        case hostSelectionOffered(mimeTypes: [String])
        /// The bytes for a `hostSelectionRequest`.
        case hostSelectionData(token: UInt32, mimeType: String, data: Data?)

        // MARK: Text input

        /// Finished text from the macOS IME. The client inserts it as if typed.
        case textCommit(window: UInt32, text: String)
        /// Text still being composed, with the selection inside it. An empty
        /// string ends the preedit.
        case textPreedit(window: UInt32, text: String, cursorBegin: Int, cursorEnd: Int)
        /// Bytes the IME wants removed around the caret before its commit —
        /// what replacing a reconverted word requires.
        case textDeleteSurrounding(window: UInt32, beforeLength: UInt32, afterLength: UInt32)

        /// Coalesce only adjacent motion, never a stop or a direction reversal.
        /// Shared by the VM and remote transports so neither can lose a gesture
        /// boundary or manufacture a stop by cancelling opposite displacements.
        public func coalescingScroll(with next: Self) -> Self? {
            guard case .pointerScroll(let window, let dx, let dy, let precise) = self,
                  case .pointerScroll(let nextWindow, let nextDX, let nextDY,
                                      let nextPrecise) = next,
                  window == nextWindow, precise == nextPrecise,
                  dx.isFinite, dy.isFinite, nextDX.isFinite, nextDY.isFinite,
                  (dx != 0 || dy != 0), (nextDX != 0 || nextDY != 0)
            else { return nil }
            func sameDirection(_ a: Double, _ b: Double) -> Bool {
                a == 0 || b == 0 || (a < 0) == (b < 0)
            }
            let x = dx + nextDX, y = dy + nextDY
            guard sameDirection(dx, nextDX), sameDirection(dy, nextDY),
                  x.isFinite, y.isFinite else { return nil }
            return .pointerScroll(window: window, dx: x, dy: y, isPrecise: precise)
        }
    }

    public enum ToplevelState: String, Codable, Sendable, Equatable {
        case maximized
        case fullscreen
        case resizing
        case activated
    }

    public enum PointerButton: String, Codable, Sendable {
        case left
        case right
        case middle
    }

    /// Encoded as a bare little-endian bit field by WindowWire.
    public struct Modifiers: OptionSet, Codable, Sendable {
        public let rawValue: UInt32
        public init(rawValue: UInt32) { self.rawValue = rawValue }

        public init(from decoder: Decoder) throws {
            rawValue = try decoder.singleValueContainer().decode(UInt32.self)
        }

        public func encode(to encoder: Encoder) throws {
            var container = encoder.singleValueContainer()
            try container.encode(rawValue)
        }

        public static let shift = Modifiers(rawValue: 1 << 0)
        public static let control = Modifiers(rawValue: 1 << 1)
        /// Linux calls it Alt; macOS calls it Option. Same physical key.
        public static let alt = Modifiers(rawValue: 1 << 2)
        /// Linux calls it Super; macOS calls it Command.
        public static let logo = Modifiers(rawValue: 1 << 3)
        public static let capsLock = Modifiers(rawValue: 1 << 4)
    }
}
