import Foundation

/// The window protocol between the guest's Wayland translator and the host.
///
/// This is deliberately *not* Wayland. The guest side runs the Wayland protocol
/// state machine — surface roles, commit atomicity, buffer release timing — and
/// forwards only what macOS needs to put a window on screen. Everything the
/// design says NativePipe does not do is absent here: no scene graph, no
/// stacking order, no shadows, no decorations, no compositing, no vsync.
///
/// The mapping is one-to-one and stays that way:
///
///     wl_surface   #17  ->  NativeSurface #17
///     xdg_toplevel #23  ->  NativeWindow  #23  ->  NSWindow *
public enum Windowing {}

// MARK: - Guest -> host

extension Windowing {
    /// Things the guest's translator reports upward.
    public enum GuestEvent: Codable, Sendable {
        case surfaceCreated(surface: UInt32)
        case surfaceDestroyed(surface: UInt32)

        /// A surface took the toplevel role. No `NSWindow` is created yet: one
        /// appears on the first commit that carries a frame, because a window
        /// shown before it has content flashes empty.
        case toplevelCreated(window: UInt32, surface: UInt32)
        case toplevelDestroyed(window: UInt32)

        /// A surface took the popup role: a menu, dropdown or tooltip anchored to
        /// another window. Position is already resolved against the positioner
        /// the client supplied, relative to the parent's xdg window geometry.
        case popupCreated(
            window: UInt32, surface: UInt32, parent: UInt32,
            x: Int, y: Int, width: Int, height: Int)
        case popupDestroyed(window: UInt32)

        /// A surface became a child of another, at an offset in the parent's
        /// surface-local coordinates. Not a window: it is part of the parent's
        /// contents, and the host gives it a layer inside the parent's view so
        /// CoreAnimation does the compositing.
        case subsurfaceCreated(surface: UInt32, parent: UInt32, x: Int, y: Int)
        case subsurfaceMoved(surface: UInt32, x: Int, y: Int)
        case subsurfaceDestroyed(surface: UInt32)

        /// The surface supplied to wl_data_device.start_drag. It is a transient,
        /// pointer-following image rather than a window or subsurface. Nil ends
        /// the overlay when the drag completes or is cancelled.
        case dragIconChanged(surface: UInt32?)

        case titleChanged(window: UInt32, title: String)
        case appIDChanged(window: UInt32, appID: String)
        /// Whether AppKit owns the window chrome. If false, the client draws
        /// CSD over a transparent full-size AppKit title bar.
        case decorationModeChanged(window: UInt32, serverSide: Bool)
        case parentChanged(window: UInt32, parent: UInt32?)
        case sizeConstraintsChanged(window: UInt32, minimum: Size?, maximum: Size?)

        /// The client acknowledged a configure and attached matching content.
        case committed(surface: UInt32, frame: Frame)

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
        case selectionData(token: UInt32, mimeType: String, base64: String?)
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
        /// Buffer scale, so a 2x surface reports its pixel size here and its
        /// point size after division.
        public var scale: Int
        /// The visible xdg-shell window inside the wl_surface, in logical
        /// surface coordinates. CSD clients commonly leave transparent shadow
        /// margins outside this rectangle; those margins are not part of the
        /// native window size or its input coordinate origin.
        public var windowGeometry: Rect?
        /// Damage in surface-local pixels. Empty means the whole surface.
        public var damage: [Rect]
        /// How this frame was produced. CPU is a guest memcpy into an
        /// IOSurface. GPU is a Venus image that already lives in MoltenVK
        /// on the host — the resource id is only a name, not a copy.
        public var source: FrameSourceKind

        public init(
            resourceID: UInt32, width: Int, height: Int, bytesPerRow: Int,
            format: PixelFormat, scale: Int = 1, windowGeometry: Rect? = nil,
            damage: [Rect] = [], source: FrameSourceKind = .cpu
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
        }

        enum CodingKeys: String, CodingKey {
            case resourceID, width, height, bytesPerRow, format, scale
            case windowGeometry, damage, source
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
        }
    }

    /// Guest compositor's classification of a committed buffer.
    public enum FrameSourceKind: String, Codable, Sendable {
        /// `wl_shm`: compositor copied into a host IOSurface.
        case cpu
        /// Venus / linux-dmabuf: the virtio-gpu resource *is* the host
        /// MoltenVK image. No second allocation.
        case gpu
    }

    public enum PixelFormat: String, Codable, Sendable {
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
    public enum HostCommand: Codable, Sendable {
        /// The window changed size or state. The client redraws and acks; the
        /// host does not stretch the old frame in the meantime, which is what
        /// keeps resizing from looking rubbery.
        case configure(window: UInt32, size: Size, states: [ToplevelState], serial: UInt32)
        /// User clicked the close button. This is a request — the client decides.
        case close(window: UInt32)

        /// Dismiss a popup: the click went elsewhere, or the parent lost focus.
        /// Like close, this is a request; the client tears the popup down.
        case dismissPopup(window: UInt32)
        /// Backing scale of the screen the window is on, which changes when the
        /// user drags it between displays.
        case scaleChanged(window: UInt32, scale: Int)

        case keyboardFocus(window: UInt32?)
        case key(window: UInt32, keycode: UInt32, pressed: Bool, modifiers: Modifiers)
        case pointerEntered(window: UInt32, x: Double, y: Double)
        case pointerMoved(window: UInt32, x: Double, y: Double)
        case pointerLeft(window: UInt32)
        case pointerButton(window: UInt32, button: PointerButton, pressed: Bool)
        case pointerScroll(window: UInt32, dx: Double, dy: Double, isPrecise: Bool)

        // MARK: Clipboard — the mirror image of the guest's three events.

        /// The host is pasting into macOS and needs the guest selection's bytes.
        case selectionRequest(token: UInt32, mimeType: String)
        /// The macOS pasteboard changed and now offers these MIME types. An
        /// empty list clears the selection inside the guest.
        case hostSelectionOffered(mimeTypes: [String])
        /// The bytes for a `hostSelectionRequest`.
        case hostSelectionData(token: UInt32, mimeType: String, base64: String?)

        // MARK: Text input

        /// Finished text from the macOS IME. The client inserts it as if typed.
        case textCommit(window: UInt32, text: String)
        /// Text still being composed, with the selection inside it. An empty
        /// string ends the preedit.
        case textPreedit(window: UInt32, text: String, cursorBegin: Int, cursorEnd: Int)
        /// Bytes the IME wants removed around the caret before its commit —
        /// what replacing a reconverted word requires.
        case textDeleteSurrounding(window: UInt32, beforeLength: UInt32, afterLength: UInt32)
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

    /// Encoded as a bare number, not as the `{"rawValue": …}` object Swift
    /// synthesises for an OptionSet. The other end reads it with a JSON integer
    /// accessor, which would silently see zero and drop every modifier.
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
