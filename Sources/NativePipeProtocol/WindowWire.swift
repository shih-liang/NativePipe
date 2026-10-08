import NativePipeStrings
import Foundation
import QuartzCore

/// Binary payloads for every window-channel message.
///
/// NPIP supplies only bounded length framing. Integer fields are explicitly
/// little endian; this is a wire format, never a Swift struct memory dump.
public enum WindowWire {
    public static let windowProtocolVersion: UInt32 = 15
    public static let motionMagic: [UInt8] = Array("NPMO".utf8)
    public static let motionPayloadSize = 16
    public static let scrollMagic: [UInt8] = Array("NPSC".utf8)
    public static let configureMagic: [UInt8] = Array("NPCF".utf8)
    public static let frameTimingMagic: [UInt8] = Array("NPFT".utf8)
    public static let popupConfigureMagic: [UInt8] = Array("NPPF".utf8)
    public static let sceneMagic: [UInt8] = Array("NPSN".utf8)
    public static let lifecycleMagic: [UInt8] = Array("NPW2".utf8)
    public static let sceneVersion: UInt16 = 4
    public static let sceneHeaderSize = 100
    public static let sceneLayerSize = 88
    public static let maximumSceneLayers = 128
    public static let maximumFieldSize = 8 * 1024 * 1024
    /// Leaves room for NPW2 framing and the MIME field inside WireFormat's
    /// eight-MiB message limit. The guest compositor uses the same ceiling.
    public static let maximumClipboardDataSize = 7 * 1024 * 1024
    public static let maximumCollectionCount = 4096
    /// A notification has at most this many actions; more is malformed.
    public static let maximumNotificationActions = 8
    /// UTF-8 byte budgets shared with guest/compositor/notify_dbus.h.
    public static let maximumNotificationNameSize = 128
    public static let maximumNotificationSummarySize = 512
    public static let maximumNotificationBodySize = 4096
    public static let maximumNotificationActionSize = 128
    public static let maximumMIMETypes = 24

    public enum DecodeError: LocalizedError, Equatable {
        case notBinaryScene
        case truncated
        case unsupportedVersion(UInt16)
        case unsupportedWindowVersion(UInt32)
        case malformed

        public var errorDescription: String? {
            switch self {
            // Neither version error says which side is newer, so neither
            // guesses: the fix is the same either way.
            case .unsupportedWindowVersion, .unsupportedVersion:
                NPText("The NativePipe compositor on Linux doesn’t match this version of NativePipe. Update NativePipe on both computers to the same version, then reconnect.")

            case .notBinaryScene, .truncated, .malformed:
                NPText("The NativePipe compositor sent display data that this Mac couldn’t read. Reconnect to continue.")
            }
        }
    }

    public enum EncodeError: Error, Equatable {
        case invalidValue
    }

    /// Decode one guest-to-host scene snapshot. Every count and offset is
    /// validated before a value is read; callers can safely reject malformed or
    /// newer messages without losing NPIP stream framing.
    public static func guestEvent(from payload: Data) throws -> Windowing.GuestEvent {
        if payload.count >= 4, Array(payload.prefix(4)) == lifecycleMagic {
            return try lifecycleEvent(from: payload)
        }
        guard payload.count >= 4,
              Array(payload.prefix(4)) == sceneMagic else {
            throw DecodeError.notBinaryScene
        }
        // CACurrentMediaTime and MTLDrawable.presentedTime share this host
        // clock. Record receipt on the decoder's thread, before UI queueing.
        let receivedHostTime = UInt64(CACurrentMediaTime() * 1_000_000_000)
        var reader = Reader(payload)
        try reader.skip(4)
        let version: UInt16 = try reader.integer()
        guard version == sceneVersion else { throw DecodeError.unsupportedVersion(version) }
        let headerSize = Int(try reader.integer() as UInt16)
        let totalSize = Int(try reader.integer() as UInt32)
        guard headerSize == sceneHeaderSize, totalSize == payload.count,
              totalSize >= sceneHeaderSize else { throw DecodeError.malformed }

        let surface: UInt32 = try reader.integer()
        let presentationID: UInt32 = try reader.integer()
        let width = Int(try reader.integer() as UInt32)
        let height = Int(try reader.integer() as UInt32)
        let scale = Int(try reader.integer() as UInt32)
        let geometry = Windowing.Rect(
            x: Int(try reader.integer() as Int32),
            y: Int(try reader.integer() as Int32),
            width: Int(try reader.integer() as Int32),
            height: Int(try reader.integer() as Int32))
        let layerCount = Int(try reader.integer() as UInt32)
		let flags = try reader.integer() as UInt32
		let damage = Windowing.Rect(
			x: Int(try reader.integer() as Int32),
			y: Int(try reader.integer() as Int32),
			width: Int(try reader.integer() as Int32),
			height: Int(try reader.integer() as Int32))
		let configureSerial: UInt32 = try reader.integer()
        let sessionID: UInt64 = try reader.integer()
        let clockEpoch: UInt64 = try reader.integer()
        let guestSendTime: UInt64 = try reader.integer()

        guard sessionID != 0, clockEpoch != 0, guestSendTime != 0,
              surface != 0, presentationID != 0,
              width > 0, height > 0, scale >= 1, scale <= 4,
              geometry.width > 0, geometry.height > 0,
			  flags & ~1 == 0,
			  damage.x >= 0, damage.y >= 0,
			  damage.width >= 0, damage.height >= 0,
			  (damage.width == 0) == (damage.height == 0),
			  damage.x <= width - damage.width,
			  damage.y <= height - damage.height,
              layerCount > 0, layerCount <= maximumSceneLayers,
              layerCount <= (Int.max - sceneHeaderSize) / sceneLayerSize,
              sceneHeaderSize + layerCount * sceneLayerSize == totalSize
        else { throw DecodeError.malformed }

        var layers: [Windowing.SceneLayer] = []
        layers.reserveCapacity(layerCount)
        for _ in 0..<layerCount {
            let layerSurface: UInt32 = try reader.integer()
            let resourceID: UInt32 = try reader.integer()
            let layerWidth = Int(try reader.integer() as UInt32)
            let layerHeight = Int(try reader.integer() as UInt32)
            let bytesPerRow = Int(try reader.integer() as UInt32)
            let formatRaw: UInt16 = try reader.integer()
            let flags: UInt16 = try reader.integer()
            let transformRaw: UInt32 = try reader.integer()
            _ = try reader.integer() as UInt32
            let destination = try reader.floatRect()
            let source = try reader.floatRect()
            let clip = try reader.floatRect()
            let alpha = try reader.float()
            _ = try reader.integer() as UInt32

            let format: Windowing.PixelFormat
            switch formatRaw {
            case 1: format = .bgra8888
            case 2: format = .bgrx8888
            case 3: format = .rgba8888
            default: throw DecodeError.malformed
            }
            guard let transform = Windowing.BufferTransform(rawValue: transformRaw),
                  layerSurface != 0, resourceID != 0,
                  layerWidth > 0, layerHeight > 0,
                  bytesPerRow >= layerWidth * 4,
                  alpha.isFinite, alpha >= 0, alpha <= 1,
                  destination.isValidPositive, source.isValidPositive,
                  clip.isValidPositive,
                  source.x >= 0, source.y >= 0,
                  source.x + source.width <= Double(layerWidth) + 0.01,
                  source.y + source.height <= Double(layerHeight) + 0.01
            else { throw DecodeError.malformed }

            layers.append(Windowing.SceneLayer(
                surface: layerSurface, resourceID: resourceID,
                width: layerWidth, height: layerHeight,
                bytesPerRow: bytesPerRow, format: format,
                destination: destination, sourcePixels: source, clip: clip,
                alpha: alpha, opaque: flags & 1 != 0, transform: transform))
        }
        guard reader.isAtEnd else { throw DecodeError.malformed }
        return .sceneCommitted(scene: Windowing.SceneSnapshot(
            surface: surface, presentationID: presentationID,
            width: width, height: height, scale: scale,
			windowGeometry: geometry, configureSerial: configureSerial, layers: layers,
			damage: damage.width > 0 ? [damage] : [],
            presentationContext: .init(sessionID: sessionID, clockEpoch: clockEpoch,
                                       guestSendTimeNanoseconds: guestSendTime),
            receivedHostTimeNanoseconds: receivedHostTime))
    }

    private static func lifecycleEvent(from payload: Data) throws -> Windowing.GuestEvent {
        let receivedHostTime = UInt64(CACurrentMediaTime() * 1_000_000_000)
        var reader = Reader(payload)
        try reader.skip(4)
        let direction: UInt8 = try reader.integer()
        let opcode: UInt8 = try reader.integer()
        let reserved: UInt16 = try reader.integer()
        guard direction == 1, reserved == 0 else { throw DecodeError.malformed }

        func finished(_ event: Windowing.GuestEvent) throws -> Windowing.GuestEvent {
            guard reader.isAtEnd else { throw DecodeError.malformed }
            return event
        }
        switch opcode {
        case 1:
            let sessionID: UInt32 = try reader.integer()
            let protocolVersion: UInt32 = try reader.integer()
            guard sessionID != 0 else { throw DecodeError.malformed }
            guard protocolVersion == windowProtocolVersion else {
                throw DecodeError.unsupportedWindowVersion(protocolVersion)
            }
            return try finished(.channelReady(
                sessionID: sessionID, protocolVersion: protocolVersion))
        case 2:
            return try finished(.surfaceCreated(surface: reader.integer()))
        case 3:
            return try finished(.surfaceDestroyed(surface: reader.integer()))
        case 4:
            return try finished(.surfaceUnmapped(surface: reader.integer()))
        case 5:
            return try finished(.toplevelCreated(
                window: reader.integer(), surface: reader.integer()))
        case 6:
            return try finished(.toplevelDestroyed(window: reader.integer()))
        case 7:
            let window: UInt32 = try reader.integer()
            let surface: UInt32 = try reader.integer()
            let parent: UInt32 = try reader.integer()
            let x = Int(try reader.integer() as Int32)
            let y = Int(try reader.integer() as Int32)
            let width = Int(try reader.integer() as Int32)
            let height = Int(try reader.integer() as Int32)
            guard width > 0, height > 0 else { throw DecodeError.malformed }
            return try finished(.popupCreated(
                window: window, surface: surface, parent: parent,
                x: x, y: y, width: width, height: height))
        case 8:
            let window: UInt32 = try reader.integer()
            let x = Int(try reader.integer() as Int32)
            let y = Int(try reader.integer() as Int32)
            let width = Int(try reader.integer() as Int32)
            let height = Int(try reader.integer() as Int32)
            guard width > 0, height > 0 else { throw DecodeError.malformed }
            return try finished(.popupRepositioned(
                window: window, x: x, y: y, width: width, height: height))
        case 9:
            return try finished(.popupDestroyed(window: reader.integer()))
        case 13:
            let surface: UInt32 = try reader.integer()
            return try finished(.dragIconChanged(surface: surface == 0 ? nil : surface))
        case 14:
            let surface: UInt32 = try reader.integer()
            let hotspotX = Int(try reader.integer() as Int32)
            let hotspotY = Int(try reader.integer() as Int32)
            let pixelScale = Int(try reader.integer() as UInt32)
            guard (1...8).contains(pixelScale) else { throw DecodeError.malformed }
            return try finished(.cursorChanged(
                surface: surface == 0 ? nil : surface,
                hotspotX: hotspotX, hotspotY: hotspotY, pixelScale: pixelScale))
        case 15:
            let raw: UInt32 = try reader.integer()
            guard let shape = Windowing.CursorShape(rawValue: raw) else {
                throw DecodeError.malformed
            }
            return try finished(.cursorShapeChanged(shape: shape))
        case 16:
            return try finished(.titleChanged(
                window: reader.integer(), title: reader.string()))
        case 17:
            return try finished(.appIDChanged(
                window: reader.integer(), appID: reader.string()))
        case 18:
            return try finished(.decorationModeChanged(
                window: reader.integer(), serverSide: reader.boolean()))
        case 19:
            let window: UInt32 = try reader.integer()
            let parent: UInt32? = try reader.boolean() ? reader.integer() : nil
            return try finished(.parentChanged(window: window, parent: parent))
        case 20:
            let window: UInt32 = try reader.integer()
            let minimum: Windowing.Size?
            if try reader.boolean() {
                minimum = Windowing.Size(
                    width: Int(try reader.integer() as Int32),
                    height: Int(try reader.integer() as Int32))
            } else { minimum = nil }
            let maximum: Windowing.Size?
            if try reader.boolean() {
                maximum = Windowing.Size(
                    width: Int(try reader.integer() as Int32),
                    height: Int(try reader.integer() as Int32))
            } else { maximum = nil }
            return try finished(.sizeConstraintsChanged(
                window: window, minimum: minimum, maximum: maximum))
        case 21:
            let surface: UInt32 = try reader.integer()
            guard surface != 0 else { throw DecodeError.malformed }
            return try finished(.committed(
                surface: surface, frame: reader.frame(receivedHostTime: receivedHostTime)))
        case 22:
            let surface: UInt32 = try reader.integer()
            let presentationID: UInt32 = try reader.integer()
            guard surface != 0, presentationID != 0 else {
                throw DecodeError.malformed
            }
            return try finished(.frameCallbackRequested(
                surface: surface, presentationID: presentationID))
        case 23:
            return try finished(.interactiveMoveRequested(
                window: reader.integer(), serial: reader.integer()))
        case 24:
            let window: UInt32 = try reader.integer()
            let raw: UInt32 = try reader.integer()
            let serial: UInt32 = try reader.integer()
            guard raw != 0, raw & ~0xf == 0 else { throw DecodeError.malformed }
            return try finished(.interactiveResizeRequested(
                window: window, edges: Windowing.ResizeEdge(rawValue: raw),
                serial: serial))
        case 25:
            return try finished(.fullscreenRequested(
                window: reader.integer(), enabled: reader.boolean()))
        case 26:
            return try finished(.maximizeRequested(
                window: reader.integer(), enabled: reader.boolean()))
        case 27:
            return try finished(.minimizeRequested(window: reader.integer()))
        case 28:
            return try finished(.selectionOffered(mimeTypes: reader.strings()))
        case 29:
            return try finished(.selectionData(
                token: reader.integer(), mimeType: reader.string(),
                data: reader.optionalData()))
        case 30:
            return try finished(.hostSelectionRequest(
                token: reader.integer(), mimeType: reader.string()))
        case 31:
            let window: UInt32 = try reader.integer()
            let epoch: UInt32 = try reader.integer()
            guard window != 0, epoch != 0 else { throw DecodeError.malformed }
            return try finished(.textInputEnabled(window: window, epoch: epoch, enabled: reader.boolean()))
        case 32:
            return try finished(.textInputCursorRect(
                window: reader.integer(),
                x: Int(try reader.integer() as Int32),
                y: Int(try reader.integer() as Int32),
                width: Int(try reader.integer() as Int32),
                height: Int(try reader.integer() as Int32)))
        case 33:
            let window: UInt32 = try reader.integer()
            let text = try reader.string()
            let cursor = Int(try reader.integer() as Int32)
            let anchor = Int(try reader.integer() as Int32)
            guard text.utf8.count <= 4000, textByteIndex(cursor, in: text),
                  textByteIndex(anchor, in: text) else { throw DecodeError.malformed }
            return try finished(.textInputSurroundingText(window: window, text: text, cursor: cursor, anchor: anchor))
        case 46:
            let window: UInt32 = try reader.integer()
            let hints: UInt32 = try reader.integer()
            let purpose: UInt32 = try reader.integer()
            let cause: UInt32 = try reader.integer()
            guard window != 0, hints & ~0x3ff == 0, purpose <= 13, cause <= 1 else { throw DecodeError.malformed }
            return try finished(.textInputContentType(window: window, hints: hints, purpose: purpose, changeCause: cause))
        case 35:
            let placement = Windowing.PopupPlacement(
                window: try reader.integer(), parent: try reader.integer(),
                x: Int(try reader.integer() as Int32),
                y: Int(try reader.integer() as Int32),
                flippedX: Int(try reader.integer() as Int32),
                flippedY: Int(try reader.integer() as Int32),
                width: Int(try reader.integer() as Int32),
                height: Int(try reader.integer() as Int32),
                adjustment: try reader.integer(), token: try reader.integer(),
                reactive: try reader.boolean())
            guard placement.window != 0, placement.width > 0,
                  placement.height > 0, placement.adjustment & ~0x3f == 0
            else { throw DecodeError.malformed }
            return try finished(.popupPlacementRequested(placement))
        case 37:
            let raw: UInt32 = try reader.integer()
            guard let action = FileDragMessage.Action(rawValue: raw), raw >= 101 else { throw DecodeError.malformed }
            let token: UInt32 = try reader.integer(), window: UInt32 = try reader.integer()
            let x = Double(bitPattern: try reader.integer() as UInt64)
            let y = Double(bitPattern: try reader.integer() as UInt64)
            let data = try reader.optionalData()
            guard token != 0, x.isFinite, y.isFinite, (data?.count ?? 0) <= 1024 * 1024 else { throw DecodeError.malformed }
            return try finished(.fileDrag(.init(action, token: token, window: window, x: x, y: y, data: data)))
        case 36:
            return try finished(.forceQuitCapabilityChanged(
                window: reader.integer(), supported: reader.boolean()))
        case 38, 39:
            var id: UInt32?, revision: UInt64?
            do {
                let value: UInt32 = try reader.integer()
                id = value == 0 ? nil : value
                let serial: UInt64 = try reader.integer()
                revision = serial == 0 ? nil : serial
                guard let id, let revision else { throw DecodeError.malformed }
                if opcode == 39 { return try finished(.notificationClosed(id: id, revision: revision)) }
                let rawUrgency: UInt8 = try reader.integer()
                guard let urgency = Windowing.GuestNotification.Urgency(rawValue: rawUrgency) else {
                    throw DecodeError.malformed
                }
                let timeout = Int(try reader.integer() as Int32)
                guard timeout >= -1 else { throw DecodeError.malformed }
                let appName = try reader.string(maximumBytes: maximumNotificationNameSize)
                let desktopEntry = try reader.string(maximumBytes: maximumNotificationNameSize)
                let summary = try reader.string(maximumBytes: maximumNotificationSummarySize)
                let body = try reader.string(maximumBytes: maximumNotificationBodySize)
                let count = Int(try reader.integer() as UInt32)
                guard count <= maximumNotificationActions else { throw DecodeError.malformed }
                var actions: [Windowing.GuestNotification.Action] = []
                for _ in 0..<count {
                    actions.append(.init(
                        key: try reader.string(maximumBytes: maximumNotificationActionSize),
                        label: try reader.string(maximumBytes: maximumNotificationActionSize)))
                }
                return try finished(.notificationPosted(.init(
                    id: id, revision: revision, urgency: urgency, timeoutMilliseconds: timeout,
                    appName: appName, desktopEntry: desktopEntry,
                    summary: summary, body: body, actions: actions)))
            } catch {
                // Notifications do not own the surface graph. Their text or
                // actions may be refused without reconnecting the display.
                return .notificationRejected(id: id, revision: revision)
            }
        case 42:
            do { return try finished(.notificationBacklogReset) }
            catch { return .notificationRejected(id: nil, revision: nil) }
        case 43:
            let window: UInt32 = try reader.integer()
            let origin: UInt32 = try reader.integer()
            let age: UInt32 = try reader.integer()
            guard window != 0, origin != 0, age <= 5_000 else { throw DecodeError.malformed }
            return try finished(.activationRequested(
                window: window, originWindow: origin, inputAgeMilliseconds: age))
        case 44:
            let token: UInt32 = try reader.integer()
            let session: UInt64 = try reader.integer()
            let epoch: UInt64 = try reader.integer()
            guard token != 0, session != 0, epoch != 0 else { throw DecodeError.malformed }
            return try finished(.presentationClockRequested(token: token, sessionID: session, clockEpoch: epoch))
        case 47:
            let session: UInt64 = try reader.integer()
            let epoch: UInt64 = try reader.integer()
            let surface: UInt32 = try reader.integer()
            let scene: UInt32 = try reader.integer()
            guard session != 0, epoch != 0, surface != 0, scene != 0 else { throw DecodeError.malformed }
            return try finished(.presentationFeedbackAcknowledged(
                sessionID: session, clockEpoch: epoch, surface: surface, presentationID: scene))
        case 48, 49:
            let session: UInt64 = try reader.integer()
            let token: UInt32 = try reader.integer()
            guard session != 0, token != 0 else { throw DecodeError.malformed }
            return try finished(opcode == 48 ? .presentationPauseReached(sessionID: session, token: token)
                                            : .presentationDrained(sessionID: session, token: token))
        case 50:
            let session: UInt64 = try reader.integer()
            let epoch: UInt64 = try reader.integer()
            let token: UInt32 = try reader.integer()
            guard session != 0, epoch != 0, token != 0 else { throw DecodeError.malformed }
            return try finished(.presentationResumed(sessionID: session, clockEpoch: epoch, token: token))
        case 40, 41:
            var token: UInt32?
            do {
                let value: UInt32 = try reader.integer()
                token = value == 0 ? nil : value
                guard let token else { throw DecodeError.malformed }
                if opcode == 41 { return try finished(.hostOpenCancelled(token: token)) }
                guard let frame = try reader.data(maximumSize: HostOpenWire.headerSize + HostOpenWire.maximumPayload) else {
                    throw DecodeError.malformed
                }
                let request = try HostOpenWire.decodeRequest(from: frame)
                return try finished(.hostOpenRequested(token: token, request: request))
            } catch {
                return .hostOpenRejected(token: token)
            }
        default:
            throw DecodeError.malformed
        }
    }

    /// Encodes any host command. High-rate cases retain their compact fixed
    /// records; all other cases use the same bounded NPW2 field encoding as C.
    public static func commandPayload(for command: Windowing.HostCommand) throws -> Data {
        if let fast = fastPayload(for: command) { return fast }

        let opcode: UInt8
        switch command {
        case .fileDrag: opcode = 28
        case .notificationClosed: opcode = 29
        case .notificationAction: opcode = 30
        case .hostOpenResponse: opcode = 31
        case .presentationClockSample: opcode = 35
        case .presentationFeedback: opcode = 36
        case .sceneClockSample: opcode = 37
        case .presentationPause: opcode = 38
        case .presentationDrain: opcode = 39
        case .presentationResume: opcode = 40
        case .configure: opcode = 1
        case .close: opcode = 2
        case .dismissPopup: opcode = 3
        case .scaleChanged: opcode = 4
        case .keyboardFocus: opcode = 5
        case .key: opcode = 6
        case .pointerEntered: opcode = 7
        case .pointerMoved: opcode = 8
        case .pointerLeft: opcode = 9
        case .pointerButton: opcode = 10
        case .pointerScroll: opcode = 11
        case .windowState: opcode = 32
        case .framePresented: opcode = 12
        case .selectionRequest: opcode = 14
        case .hostSelectionOffered: opcode = 15
        case .hostSelectionData: opcode = 16
        case .textCommit: opcode = 17
        case .textPreedit: opcode = 18
        case .textDeleteSurrounding: opcode = 19
        case .textEdit: opcode = 33
        case .frameReleased: opcode = 20
        case .forceQuit: opcode = 21
        case .configurePopup: opcode = 22
        case .outputsChanged: opcode = 23
        case .windowOutputChanged: opcode = 24
        case .inputPreferences: opcode = 25
        case .captureFrame: opcode = 26
        case .applicationRequest: opcode = 27
        }
        var payload = Data(lifecycleMagic)
        payload.append(2)
        payload.append(opcode)
        append(UInt16(0), to: &payload)

        switch command {
        case .presentationClockSample(let token, let session, let epoch, let time):
            guard token != 0, session != 0, epoch != 0, time != 0 else { throw EncodeError.invalidValue }
            append(token, to: &payload)
            append(session, to: &payload)
            append(epoch, to: &payload)
            append(time, to: &payload)
        case .sceneClockSample(let session, let epoch, let surface, let id, let guest, let host):
            guard session != 0, epoch != 0, surface != 0, id != 0, guest != 0, host != 0 else {
                throw EncodeError.invalidValue
            }
            append(session, to: &payload)
            append(epoch, to: &payload)
            append(surface, to: &payload)
            append(id, to: &payload)
            append(guest, to: &payload)
            append(host, to: &payload)
        case .presentationPause(let session, let token), .presentationDrain(let session, let token),
             .presentationResume(let session, let token):
            guard session != 0, token != 0 else { throw EncodeError.invalidValue }
            append(session, to: &payload)
            append(token, to: &payload)
        case .presentationFeedback(let session, let epoch, let surface, let id, let time, let refresh, let output):
            guard session != 0, epoch != 0, surface != 0, id != 0 else { throw EncodeError.invalidValue }
            append(session, to: &payload)
            append(epoch, to: &payload)
            append(surface, to: &payload)
            append(id, to: &payload)
            append(time, to: &payload)
            append(refresh, to: &payload)
            append(output, to: &payload)
        case .hostOpenResponse(let token, let response):
            guard token != 0 else { throw EncodeError.invalidValue }
            append(token, to: &payload)
            try append(HostOpenWire.encode(response), to: &payload)
        case .notificationClosed(let id, let revision, let reason):
            guard id != 0, revision != 0 else { throw EncodeError.invalidValue }
            append(id, to: &payload)
            append(revision, to: &payload)
            append(reason.rawValue, to: &payload)
        case .notificationAction(let id, let revision, let key):
            guard id != 0, revision != 0, key.utf8.count <= maximumNotificationActionSize else { throw EncodeError.invalidValue }
            append(id, to: &payload)
            append(revision, to: &payload)
            try append(key, to: &payload)
        case .fileDrag(let message):
            guard message.action.rawValue < 101, message.token != 0,
                  message.x.isFinite, message.y.isFinite, (message.data?.count ?? 0) <= 1024 * 1024 else { throw EncodeError.invalidValue }
            append(message.action.rawValue, to: &payload)
            append(message.token, to: &payload); append(message.window, to: &payload)
            append(message.x.bitPattern, to: &payload); append(message.y.bitPattern, to: &payload)
            try append(message.data, to: &payload)
        case .applicationRequest(let token, let action, let application):
            guard token != 0, application.utf8.count <= 4096 else { throw EncodeError.invalidValue }
            append(token, to: &payload)
            append(action.rawValue, to: &payload)
            try append(application, to: &payload)
        case .close(let window), .forceQuit(let window),
             .dismissPopup(let window), .pointerLeft(let window):
            append(window, to: &payload)
        case .windowState(let window, let visible, let width, let height):
            guard window != 0, let width = Int32(exactly: width), let height = Int32(exactly: height),
                  width >= 0, height >= 0, (width == 0) == (height == 0) else { throw EncodeError.invalidValue }
            append(window, to: &payload)
            payload.append(visible ? 1 : 0)
            append(width, to: &payload)
            append(height, to: &payload)
        case .scaleChanged(let window, let scale):
            guard let scale = Int32(exactly: scale), scale > 0 else {
                throw EncodeError.invalidValue
            }
            append(window, to: &payload)
            append(scale, to: &payload)
        case .keyboardFocus(let window):
            append(window ?? 0, to: &payload)
        case .key(let window, let keycode, let pressed, let modifiers):
            append(window, to: &payload)
            append(keycode, to: &payload)
            payload.append(pressed ? 1 : 0)
            append(modifiers.rawValue, to: &payload)
        case .pointerEntered(let window, let x, let y):
            guard x.isFinite, y.isFinite else { throw EncodeError.invalidValue }
            append(window, to: &payload)
            append(x.bitPattern, to: &payload)
            append(y.bitPattern, to: &payload)
        case .pointerButton(let window, let button, let pressed):
            append(window, to: &payload)
            switch button {
            case .left: payload.append(1)
            case .right: payload.append(2)
            case .middle: payload.append(3)
            }
            payload.append(pressed ? 1 : 0)
        case .selectionRequest(let token, let mimeType):
            append(token, to: &payload)
            try append(mimeType, to: &payload)
        case .hostSelectionOffered(let mimeTypes):
            guard mimeTypes.count <= maximumMIMETypes else {
                throw EncodeError.invalidValue
            }
            try append(mimeTypes, to: &payload)
        case .hostSelectionData(let token, let mimeType, let data):
            guard data.map({ $0.count <= maximumClipboardDataSize }) ?? true else {
                throw EncodeError.invalidValue
            }
            append(token, to: &payload)
            try append(mimeType, to: &payload)
            try append(data, to: &payload)
        case .textCommit(let window, let epoch, let text):
            guard window != 0, epoch != 0 else { throw EncodeError.invalidValue }
            append(window, to: &payload)
            append(epoch, to: &payload)
            try append(text, to: &payload)
        case .textPreedit(let window, let epoch, let text, let begin, let end):
            guard window != 0, epoch != 0, validPreedit(text, begin: begin, end: end),
                  let begin = Int32(exactly: begin), let end = Int32(exactly: end) else {
                throw EncodeError.invalidValue
            }
            append(window, to: &payload)
            append(epoch, to: &payload)
            try append(text, to: &payload)
            append(begin, to: &payload)
            append(end, to: &payload)
        case .textDeleteSurrounding(let window, let epoch, let before, let after):
            guard window != 0, epoch != 0 else { throw EncodeError.invalidValue }
            append(window, to: &payload)
            append(epoch, to: &payload)
            append(before, to: &payload)
            append(after, to: &payload)
        case .textEdit(let window, let epoch, let commit, let preedit, let begin, let end, let before, let after):
            guard window != 0, epoch != 0, commit != nil || preedit != nil || before != 0 || after != 0,
                  preedit.map({ validPreedit($0, begin: begin, end: end) }) ?? (begin == 0 && end == 0),
                  let begin = Int32(exactly: begin), let end = Int32(exactly: end) else { throw EncodeError.invalidValue }
            append(window, to: &payload)
            append(epoch, to: &payload)
            append(before, to: &payload); append(after, to: &payload)
            payload.append(commit == nil ? 0 : 1)
            if let commit { try append(commit, to: &payload) }
            payload.append(preedit == nil ? 0 : 1)
            if let preedit { try append(preedit, to: &payload) }
            append(begin, to: &payload); append(end, to: &payload)
        case .frameReleased(let surface, let presentationID):
            append(surface, to: &payload)
            append(presentationID, to: &payload)
        case .captureFrame(let surface):
            append(surface, to: &payload)
        case .outputsChanged(let displays):
            guard displays.count <= 32 else { throw EncodeError.invalidValue }
            append(UInt32(displays.count), to: &payload)
            for display in displays {
                append(display.id, to: &payload)
                try append(display.name, to: &payload)
                for value in [
                    display.x, display.y, display.width, display.height,
                    display.pixelWidth, display.pixelHeight,
                    display.physicalWidthMM, display.physicalHeightMM,
                    display.scale, display.refreshMilliHz,
                ] {
                    guard let value = Int32(exactly: value) else {
                        throw EncodeError.invalidValue
                    }
                    append(value, to: &payload)
                }
            }
        case .windowOutputChanged(let window, let outputID):
            append(window, to: &payload)
            append(outputID ?? 0, to: &payload)
        case .inputPreferences(let layout, let repeatRate, let repeatDelay):
            guard !layout.isEmpty, layout.utf8.count <= 63,
                  layout.utf8.allSatisfy({
                    ($0 >= 48 && $0 <= 57) || ($0 >= 65 && $0 <= 90) ||
                    ($0 >= 97 && $0 <= 122) || [44, 43, 45, 95].contains($0)
                  }),
                  let rate = Int32(exactly: repeatRate), (0...100).contains(rate),
                  let delay = Int32(exactly: repeatDelay), (100...5_000).contains(delay)
            else { throw EncodeError.invalidValue }
            try append(layout, to: &payload)
            append(rate, to: &payload)
            append(delay, to: &payload)
        case .configure, .configurePopup, .pointerMoved, .pointerScroll,
             .framePresented:
            throw EncodeError.invalidValue // handled by fastPayload above
        }
        guard payload.count <= maximumFieldSize else { throw EncodeError.invalidValue }
        return payload
    }

    /// Encodes high-rate host state as compact fixed-size records.
    public static func fastPayload(for command: Windowing.HostCommand) -> Data? {
        switch command {
        case .pointerMoved(let window, let x, let y):
            guard x.isFinite, y.isFinite else { return nil }
            var payload = Data(motionMagic)
            append(window, to: &payload)
            append(fixed24_8(x), to: &payload)
            append(fixed24_8(y), to: &payload)
            return payload
        case .pointerScroll(let window, let dx, let dy, let precise, let inverted):
            guard dx.isFinite, dy.isFinite else { return nil }
            var payload = Data(scrollMagic)
            append(window, to: &payload)
            append(dx.bitPattern, to: &payload)
            append(dy.bitPattern, to: &payload)
            payload.append(precise ? 1 : 0)
            payload.append(contentsOf: [inverted ? 1 : 0, 0, 0])
            return payload
        case .configure(let window, let size, let states, let serial):
            guard let width = Int32(exactly: size.width),
                  let height = Int32(exactly: size.height)
            else { return nil }
            var stateBits: UInt32 = 0
            for state in states {
                switch state {
                case .maximized: stateBits |= 1 << 0
                case .fullscreen: stateBits |= 1 << 1
                case .resizing: stateBits |= 1 << 2
                case .activated: stateBits |= 1 << 3
                }
            }
            var payload = Data(configureMagic)
            append(window, to: &payload)
            append(width, to: &payload)
            append(height, to: &payload)
            append(stateBits, to: &payload)
            append(serial, to: &payload)
            return payload
        case .configurePopup(
            let window, let x, let y, let width, let height, let token):
            guard let x = Int32(exactly: x), let y = Int32(exactly: y),
                  let width = Int32(exactly: width),
                  let height = Int32(exactly: height),
                  width > 0, height > 0
            else { return nil }
            var payload = Data(popupConfigureMagic)
            append(window, to: &payload)
            append(x, to: &payload)
            append(y, to: &payload)
            append(width, to: &payload)
            append(height, to: &payload)
            append(token, to: &payload)
            return payload
        case .framePresented(let surface, let presentationID):
            var payload = Data(frameTimingMagic)
            append(UInt32(1), to: &payload)
            append(UInt32(1), to: &payload)
            append(surface, to: &payload)
            append(presentationID, to: &payload)
            return payload
        default:
            return nil
        }
    }

    /// Batches frame-clock and resource-lifetime feedback into one write while
    /// preserving every presentation id and its original order.
    public static func frameTimingPayload(
        for commands: ArraySlice<Windowing.HostCommand>
    ) -> Data? {
        guard !commands.isEmpty, commands.count <= Int(UInt32.max) else { return nil }
        var payload = Data(frameTimingMagic)
        append(UInt32(commands.count), to: &payload)
        for command in commands {
            let kind: UInt32
            let surface: UInt32
            let presentationID: UInt32
            switch command {
            case .framePresented(let value, let id):
                kind = 1
                surface = value
                presentationID = id
            case .frameReleased(let value, let id):
                kind = 2
                surface = value
                presentationID = id
            default:
                return nil
            }
            append(kind, to: &payload)
            append(surface, to: &payload)
            append(presentationID, to: &payload)
        }
        return payload
    }

    private static func fixed24_8(_ value: Double) -> Int32 {
        let scaled = (value * 256).rounded()
        if scaled <= Double(Int32.min) { return .min }
        if scaled >= Double(Int32.max) { return .max }
        return Int32(scaled)
    }

    private static func textByteIndex(_ index: Int, in text: String) -> Bool {
        let bytes = Array(text.utf8)
        return index >= 0 && index <= bytes.count && (index == bytes.count || bytes[index] & 0xc0 != 0x80)
    }

    private static func validPreedit(_ text: String, begin: Int, end: Int) -> Bool {
        (begin == -1 && end == -1) || (textByteIndex(begin, in: text) && textByteIndex(end, in: text))
    }

    private static func append<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var little = value.littleEndian
        Swift.withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
    }

    private static func append(_ value: String, to data: inout Data) throws {
        guard !value.utf8.contains(0), value.utf8.count <= maximumFieldSize else {
            throw EncodeError.invalidValue
        }
        append(UInt32(value.utf8.count), to: &data)
        data.append(contentsOf: value.utf8)
    }

    private static func append(_ value: Data?, to data: inout Data) throws {
        guard let value else {
            append(UInt32.max, to: &data)
            return
        }
        guard value.count <= maximumFieldSize else { throw EncodeError.invalidValue }
        append(UInt32(value.count), to: &data)
        data.append(value)
    }

    private static func append(_ values: [String], to data: inout Data) throws {
        guard values.count <= maximumCollectionCount else { throw EncodeError.invalidValue }
        append(UInt32(values.count), to: &data)
        for value in values { try append(value, to: &data) }
    }

    private struct Reader {
        let data: Data
        var offset = 0

        init(_ data: Data) { self.data = data }
        var isAtEnd: Bool { offset == data.count }

        mutating func skip(_ count: Int) throws {
            guard count >= 0, offset <= data.count - count else { throw DecodeError.truncated }
            offset += count
        }

        mutating func integer<T: FixedWidthInteger>() throws -> T {
            let size = MemoryLayout<T>.size
            guard offset <= data.count - size else { throw DecodeError.truncated }
            var value: T = 0
            _ = withUnsafeMutableBytes(of: &value) { destination in
                data.copyBytes(to: destination, from: offset..<(offset + size))
            }
            offset += size
            return T(littleEndian: value)
        }

        mutating func boolean() throws -> Bool {
            let value: UInt8 = try integer()
            guard value <= 1 else { throw DecodeError.malformed }
            return value == 1
        }

        mutating func float() throws -> Float {
            Float(bitPattern: try integer() as UInt32)
        }

        mutating func double() throws -> Double {
            let value = Double(bitPattern: try integer() as UInt64)
            guard value.isFinite else { throw DecodeError.malformed }
            return value
        }

        mutating func data(optional: Bool = false, maximumSize: Int = maximumFieldSize) throws -> Data? {
            let count: UInt32 = try integer()
            if optional && count == .max { return nil }
            guard count != .max, count <= maximumSize,
                  Int(count) <= data.count - offset
            else { throw DecodeError.malformed }
            let result = data.subdata(in: offset..<(offset + Int(count)))
            offset += Int(count)
            return result
        }

        mutating func optionalData() throws -> Data? {
            try data(optional: true)
        }

        mutating func string(maximumBytes: Int = maximumFieldSize) throws -> String {
            guard let bytes = try data(maximumSize: maximumBytes), !bytes.contains(0),
                  let value = String(data: bytes, encoding: .utf8)
            else { throw DecodeError.malformed }
            return value
        }

        mutating func strings() throws -> [String] {
            let count = Int(try integer() as UInt32)
            guard count <= maximumCollectionCount else { throw DecodeError.malformed }
            var values: [String] = []
            values.reserveCapacity(count)
            for _ in 0..<count { values.append(try string()) }
            return values
        }

        mutating func frame(receivedHostTime: UInt64) throws -> Windowing.Frame {
            let resourceID: UInt32 = try integer()
            let width = Int(try integer() as Int32)
            let height = Int(try integer() as Int32)
            let bytesPerRow = Int(try integer() as Int32)
            let scale = Int(try integer() as Int32)
            let formatRaw: UInt8 = try integer()
            let sourceRaw: UInt8 = try integer()
            let bitstreamEpoch: UInt16 = try integer()
            let presentationID: UInt32 = try integer()
            let flags: UInt32 = try integer()
            let damageCount = Int(try integer() as UInt32)
            guard flags & ~0x7f == 0, flags & (1 << 5) != 0,
                  damageCount <= maximumCollectionCount else {
                throw DecodeError.malformed
            }

            let viewportSource: Windowing.FloatRect?
            if flags & (1 << 0) != 0 {
                viewportSource = Windowing.FloatRect(
                    x: try double(), y: try double(),
                    width: try double(), height: try double())
            } else { viewportSource = nil }
            let viewportDestination: Windowing.Size?
            if flags & (1 << 1) != 0 {
                viewportDestination = Windowing.Size(
                    width: Int(try integer() as Int32),
                    height: Int(try integer() as Int32))
            } else { viewportDestination = nil }
            let geometry: Windowing.Rect?
            if flags & (1 << 2) != 0 {
                geometry = Windowing.Rect(
                    x: Int(try integer() as Int32),
                    y: Int(try integer() as Int32),
                    width: Int(try integer() as Int32),
                    height: Int(try integer() as Int32))
            } else { geometry = nil }
            let codec = flags & (1 << 3) != 0 ? try string() : nil
            if flags & (1 << 4) != 0 { _ = try integer() as UInt32 }

            var damage: [Windowing.Rect] = []
            damage.reserveCapacity(damageCount)
            for _ in 0..<damageCount {
                damage.append(Windowing.Rect(
                    x: Int(try integer() as Int32),
                    y: Int(try integer() as Int32),
                    width: Int(try integer() as Int32),
                    height: Int(try integer() as Int32)))
            }
            let sessionID: UInt64 = try integer()
            let clockEpoch: UInt64 = try integer()
            let guestSendTime: UInt64 = try integer()
            guard sessionID != 0, clockEpoch != 0, guestSendTime != 0 else {
                throw DecodeError.malformed
            }
            let format: Windowing.PixelFormat
            switch formatRaw {
            case 1: format = .bgra8888
            case 2: format = .bgrx8888
            case 3: format = .rgba8888
            default: throw DecodeError.malformed
            }
            let source: Windowing.FrameSourceKind
            switch sourceRaw {
            case 1: source = .cpu
            case 2: source = .gpu
            case 3: source = .encoded
            default: throw DecodeError.malformed
            }
            guard resourceID != 0, width > 0, height > 0,
                  bytesPerRow >= width * 4, scale > 0,
                  presentationID != 0,
                  viewportSource?.isValidPositive != false,
                  viewportDestination.map({ $0.width > 0 && $0.height > 0 }) != false,
                  geometry.map({ $0.width > 0 && $0.height > 0 }) != false,
                  damage.allSatisfy({ $0.width > 0 && $0.height > 0 }),
                  source == .encoded || codec == nil
            else { throw DecodeError.malformed }
            return Windowing.Frame(
                resourceID: resourceID, width: width, height: height,
                bytesPerRow: bytesPerRow, format: format, scale: scale,
                windowGeometry: geometry, damage: damage, source: source,
                codec: codec, bitstreamEpoch: bitstreamEpoch,
                presentationID: presentationID,
                viewportSource: viewportSource,
                viewportDestination: viewportDestination,
                presentationContext: .init(sessionID: sessionID, clockEpoch: clockEpoch,
                                           guestSendTimeNanoseconds: guestSendTime),
                receivedHostTimeNanoseconds: receivedHostTime,
                requestsPresentationFeedback: flags & (1 << 6) != 0)
        }

        mutating func floatRect() throws -> Windowing.FloatRect {
            Windowing.FloatRect(
                x: Double(try float()), y: Double(try float()),
                width: Double(try float()), height: Double(try float()))
        }
    }
}

private extension Windowing.FloatRect {
    var isValidPositive: Bool {
        x.isFinite && y.isFinite && width.isFinite && height.isFinite &&
        width > 0 && height > 0
    }
}
