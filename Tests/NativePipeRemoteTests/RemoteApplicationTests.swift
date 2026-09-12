import AppKit
import IOSurface
import XCTest
import NativePipeProtocol
@testable import NativePipeRemote

private final class MediaStatistics: @unchecked Sendable {
    struct Snapshot {
        var videoBytes = 0, alphaBytes = 0, videoFrames = 0
        var surfaceFrames: [UInt32: Int] = [:]
        var frameFlags: [UInt32: UInt8] = [:]
        var imageSize = CGSize.zero
        var arrivals: [(Double, Double)] = []
    }
    private let lock = NSLock()
    private var value = Snapshot()
    var snapshot: Snapshot { lock.lock(); defer { lock.unlock() }; return value }
    func reset() { lock.lock(); value = Snapshot(); lock.unlock() }
    func record(_ header: MediaWire.Header, _ payload: Data) {
        lock.lock(); defer { lock.unlock() }
        if header.codec == .h264 {
            value.videoBytes += payload.count; value.videoFrames += 1
            value.surfaceFrames[header.surfaceID, default: 0] += 1
            value.frameFlags[header.resourceID] = header.flags
            value.imageSize = CGSize(width: Int(header.width), height: Int(header.height))
            value.arrivals.append((ProcessInfo.processInfo.systemUptime, Double(header.ptsNanos) / 1e9))
        } else { value.alphaBytes += payload.count }
    }
}

final class RemoteApplicationTests: XCTestCase {
    @MainActor func testConnectionFailureCardKeepsSelectableLog() throws {
        _ = NSApplication.shared
        let log = "Checking GitHub…\nDownload failed: HTTP 404"
        let alert = RemoteApplicationDelegate.failureAlert(name: "Linux", log: log)
        XCTAssertEqual(alert.messageText, "Couldn’t Connect to Linux")
        let scroll = try XCTUnwrap(alert.accessoryView as? NSScrollView)
        let text = try XCTUnwrap(scroll.documentView as? NSTextView)
        XCTAssertEqual(text.string, log)
        XCTAssertTrue(text.isSelectable)
        XCTAssertFalse(text.isEditable)
        XCTAssertEqual(alert.buttons.first?.title, "Close")
    }

    @MainActor func testLiveRemoteAnimationLatency() throws {
        guard ProcessInfo.processInfo.environment["NATIVEPIPE_TEST_LATENCY"] == "1" else {
            throw XCTSkip("Opt-in animation and control roundtrip measurement on the selected remote")
        }
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        var failure: Error?
        Task { @MainActor in
            do { try await self.measureLiveRemoteAnimation() }
            catch { failure = error }
            app.stop(nil)
            // NSApplication.stop takes effect after nextEvent returns. This
            // local application-defined event only wakes the test's run loop.
            if let wake = NSEvent.otherEvent(with: .applicationDefined, location: .zero,
                modifierFlags: [], timestamp: 0, windowNumber: 0, context: nil,
                subtype: 0, data1: 0, data2: 0) { app.postEvent(wake, atStart: false) }
        }
        // XCTest's async executor alone does not pump AppKit window events.
        // Use the same application loop as nativepipe/RemoteHost for display.
        app.run()
        if let failure { throw failure }
    }

    @MainActor private func measureLiveRemoteAnimation() async throws {
        let env = ProcessInfo.processInfo.environment
        guard env["NATIVEPIPE_TEST_LATENCY"] == "1", let destination = env["NATIVEPIPE_TEST_REMOTE"],
              let compositor = env["NATIVEPIPE_TEST_COMPOSITOR"] else {
            throw XCTSkip("Opt-in animation and control roundtrip measurement on the selected remote")
        }
        NSApp.activate()
        var ssh = ["-o", "BatchMode=yes"]
        if let proxy = env["NATIVEPIPE_TEST_PROXY"] { ssh += ["-o", "ProxyCommand=" + proxy] }
        var application = env["NATIVEPIPE_TEST_ANIMATION"].map { [$0] } ?? ["gtk4-demo", "--run=fishbowl"]
        if env["NATIVEPIPE_TEST_OCCLUDED"] == "1", env["NATIVEPIPE_TEST_ANIMATION"] != nil { application.append("--two-windows") }
        let display = RemoteDisplayController(command: .init(destination: destination,
            application: application, sshArguments: ssh, compositor: compositor))
        display.session.onDiagnostic = { fputs($0, stderr) }
        let statistics = MediaStatistics()
        var videoBytes: Int { statistics.snapshot.videoBytes }
        var alphaBytes: Int { statistics.snapshot.alphaBytes }
        var videoFrames: Int { statistics.snapshot.videoFrames }
        var imageSize: CGSize { statistics.snapshot.imageSize }
        var arrivals: [(Double, Double)] { statistics.snapshot.arrivals }
        var decoded = 0, presented = 0, actuallyDisplayed = 0, discarded = 0
        var markers: [UInt32: Int] = [:], sceneResources: [UInt32: UInt32] = [:]
        var inputSent: Double?, inputTarget = 0, inputLatencies: [Double] = []
        var moveSent: Double?, moveLatencies: [Double] = []
        var moveNative: NSWindow?
        let receive = display.session.onMediaFrame
        display.session.onMediaFrame = { header, payload in
            statistics.record(header, payload)
            return receive?(header, payload) ?? false
        }
        display.frames.setFrameAvailableHandler { resource in
            decoded += 1
            if env["NATIVEPIPE_TEST_INPUT"] == "1",
               statistics.snapshot.frameFlags[resource, default: 0] & MediaWire.flagHasAlpha != 0,
               let surface = display.frames.surface(forResource: resource) {
                // The GTK probe has CSD alpha: these surfaces own their pixels,
                // so observing them cannot cause decoder-pool reuse or eviction.
                markers[resource] = Self.centerMarker(surface)
            }
            display.bridge.retryPendingFrames()
        }
        let send = display.bridge.output
        display.bridge.output = { command in
            if case .framePresented = command { presented += 1 }
            if env["NATIVEPIPE_TEST_RESIZE"] == "1", case .configure(let id, let size, _, let serial) = command {
                print("RESIZE_CONFIGURE id=\(id) size=\(size) serial=\(serial)")
            }
            send?(command)
        }
        let apply = display.session.onEvent
        var lastGeometry = ""
        var windowSurfaces: [UInt32: UInt32] = [:]
        display.session.onEvent = { event in
            if env["NATIVEPIPE_TEST_NATIVE_MOVE"] == "1", case .interactiveMoveRequested = event {
                if let sent = moveSent {
                    moveLatencies.append(ProcessInfo.processInfo.systemUptime - sent)
                    moveSent = nil
                }
                print("NATIVE_MOVE_REQUEST current_event=\(NSApp.currentEvent?.type.rawValue ?? 0)")
                if env["NATIVEPIPE_TEST_LEGACY_MOVE"] == "1" {
                    // Controlled A/B: reproduce the previous handoff only.
                    if let current = NSApp.currentEvent { moveNative?.performDrag(with: current) }
                    return
                }
            }
            if env["NATIVEPIPE_TEST_MOVE"] == "1", case .interactiveMoveRequested = event {
                if let sent = moveSent {
                    moveLatencies.append(ProcessInfo.processInfo.systemUptime - sent)
                    moveSent = nil
                }
                // These protocol-only inputs have no AppKit mouse-down event.
                return
            }
            if case .toplevelCreated(let window, let surface) = event { windowSurfaces[window] = surface }
            if env["NATIVEPIPE_TEST_INPUT"] == "1", case .sceneCommitted(let scene) = event {
                sceneResources[scene.presentationID] = scene.layers.first(where: { $0.surface == scene.surface })?.resourceID
            }
            if env["NATIVEPIPE_TEST_RESIZE"] == "1", case .sceneCommitted(let scene) = event {
                let geometry = "\(scene.windowGeometry) serial=\(scene.configureSerial)"
                if geometry != lastGeometry { print("RESIZE_SCENE \(geometry)"); lastGeometry = geometry }
            }
            apply?(event)
        }
        let feedback = display.bridge.onScenePresentation
        var displayInterval: UInt32 = 0
        display.bridge.onScenePresentation = { surface, id, shown, interval in
            if interval > 0 { displayInterval = interval }
            if shown { actuallyDisplayed += 1 } else { discarded += 1 }
            if let resource = sceneResources.removeValue(forKey: id), shown,
               let sent = inputSent, markers[resource] == inputTarget {
                inputLatencies.append(ProcessInfo.processInfo.systemUptime - sent)
                inputSent = nil
            }
            feedback?(surface, id, shown, interval)
        }
        defer { display.disconnect() }
        let connecting = Task { try await display.connect() }
        let timeout = Task { try? await Task.sleep(for: .seconds(20)); connecting.cancel() }
        try await connecting.value
        timeout.cancel()
        let deadline = Date().addingTimeInterval(15)
        while display.bridge.dockWindows.isEmpty && Date() < deadline {
            try await Task.sleep(for: .milliseconds(50))
        }
        let window = try XCTUnwrap(display.bridge.dockWindows.first)
        XCTAssertTrue(display.bridge.activateDockWindow(window.id))
        let native = try XCTUnwrap(NSApp.windows.first { $0.isVisible && $0.title == window.title })
        moveNative = native
        if env["NATIVEPIPE_TEST_OCCLUDED"] == "1" {
            let surface = try XCTUnwrap(windowSurfaces[window.id])
            if env["NATIVEPIPE_TEST_ANIMATION"] != nil {
                let deadline = Date().addingTimeInterval(10)
                while display.bridge.dockWindows.count < 2 && Date() < deadline { try await Task.sleep(for: .milliseconds(50)) }
                let other = try XCTUnwrap(display.bridge.dockWindows.first { $0.id != window.id })
                let otherNative = try XCTUnwrap(NSApp.windows.first { $0.title == other.title })
                otherNative.level = .floating
                otherNative.makeKeyAndOrderFront(nil)
                otherNative.orderFrontRegardless()
            }
            native.orderOut(nil)
            try await Task.sleep(for: .seconds(3))
            let before = statistics.snapshot.surfaceFrames[surface, default: 0]
            let totalBefore = videoFrames
            let sent = ProcessInfo.processInfo.systemUptime
            _ = try? await display.session.applicationClient.launch("nativepipe-latency-probe-not-installed.desktop")
            let rtt = ProcessInfo.processInfo.systemUptime - sent
            try await Task.sleep(for: .seconds(2))
            XCTAssertEqual(statistics.snapshot.surfaceFrames[surface, default: 0], before, "An invisible window must stop producing frames")
            XCTAssertLessThan(rtt, 2, "Display backpressure must not block control")
            print("REMOTE_OCCLUDED hidden_additional=\(statistics.snapshot.surfaceFrames[surface, default: 0] - before) other_frames=\(videoFrames - totalBefore) control_rtt_ms=\(rtt * 1000)")
            // A stopped first window cannot consume a connection-wide display
            // quota and prevent a second real GTK application from appearing.
            if env["NATIVEPIPE_TEST_ANIMATION"] == nil {
                try await display.session.launchApplication("org.gtk.WidgetFactory4.desktop")
            } else { XCTAssertGreaterThan(videoFrames - totalBefore, 5, "The visible second window must continue") }
            let another = Date().addingTimeInterval(15)
            while display.bridge.dockWindows.count < 2 && Date() < another {
                try await Task.sleep(for: .milliseconds(50))
            }
            let second = try XCTUnwrap(display.bridge.dockWindows.first { $0.id != window.id })
            let capture = try await display.bridge.captureDockWindow(second.id, maximumWidth: 900, maximumHeight: 700)
            XCTAssertGreaterThan(capture.png.count, 3000)
            XCTAssertTrue(display.bridge.requestCloseDockWindow(second.id))
            native.level = .floating
            native.makeKeyAndOrderFront(nil)
            native.orderFrontRegardless()
            let resumed = Date().addingTimeInterval(5)
            while statistics.snapshot.surfaceFrames[surface, default: 0] <= before + 2 && Date() < resumed {
                try await Task.sleep(for: .milliseconds(50))
            }
            XCTAssertGreaterThan(statistics.snapshot.surfaceFrames[surface, default: 0], before + 2, "Showing the hidden window must resume its latest scene")
            return
        }
        // Keep the measurement window unobscured by the test runner/editor;
        // otherwise occlusion backpressure is indistinguishable from slow net.
        native.level = .floating
        native.makeKeyAndOrderFront(nil)
        native.orderFrontRegardless()
        let visibleDeadline = Date().addingTimeInterval(3)
        while !native.occlusionState.contains(.visible) && Date() < visibleDeadline {
            try await Task.sleep(for: .milliseconds(50))
        }
        guard CGDisplayIsActive(CGMainDisplayID()) != 0, native.occlusionState.contains(.visible) else {
            throw XCTSkip("Actual display/resize measurement requires an unlocked, awake display with the test window visible")
        }
        let warming = Date().addingTimeInterval(15)
        while actuallyDisplayed < 2 && Date() < warming {
            try await Task.sleep(for: .milliseconds(50))
        }
        if env["NATIVEPIPE_TEST_NATIVE_MOVE"] == "1" {
            guard CGPreflightPostEventAccess() else { throw XCTSkip("Native drag measurement needs existing event-post access") }
            let origin = native.frame.origin
            let savedPointer = CGEvent(source: nil)?.location
            let source = CGEventSource(stateID: .hidSystemState)
            func post(_ type: CGEventType, _ point: CGPoint) {
                CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: point,
                    mouseButton: .left)?.post(tap: .cghidEventTap)
            }
            var pointer = CGPoint.zero
            var buttonHeld = false
            defer {
                if buttonHeld { post(.leftMouseUp, pointer) }
                if let savedPointer { post(.mouseMoved, savedPointer) }
            }
            var starts: [Double] = []
            for _ in 0..<3 {
                native.setFrameOrigin(origin)
                native.makeKeyAndOrderFront(nil)
                NSApp.activate()
                try await Task.sleep(for: .milliseconds(100))
                guard NSApp.isActive else { throw XCTSkip("Test window lost focus; do not send input to another application") }
                let view = try XCTUnwrap(native.contentView)
                let point = native.convertPoint(toScreen: view.convert(CGPoint(x: view.bounds.midX, y: 18), to: nil))
                let startPoint = CGPoint(x: point.x, y: CGDisplayBounds(CGMainDisplayID()).height - point.y)
                pointer = startPoint
                post(.mouseMoved, pointer)
                post(.leftMouseDown, pointer)
                buttonHeld = true
                try await Task.sleep(for: .milliseconds(30))
                let start = ProcessInfo.processInfo.systemUptime
                moveSent = start
                var firstMove: Double?
                for step in 1...100 {
                    guard NSApp.isActive else { break }
                    pointer.x = startPoint.x + CGFloat(step * 3)
                    post(.leftMouseDragged, pointer)
                    try await Task.sleep(for: .milliseconds(20))
                    let info = CGWindowListCopyWindowInfo(.optionIncludingWindow,
                        CGWindowID(native.windowNumber)) as? [[String: Any]]
                    let bounds = info?.first?[kCGWindowBounds as String] as? [String: Any]
                    let screenX = bounds?["X"] as? Double ?? Double(origin.x)
                    if firstMove == nil, abs(screenX - origin.x) > 1 {
                        firstMove = ProcessInfo.processInfo.systemUptime - start
                    }
                }
                post(.leftMouseUp, pointer)
                buttonHeld = false
                try await Task.sleep(for: .milliseconds(150))
                if let firstMove { starts.append(firstMove) }
                print("NATIVE_MOVE first_motion_ms=\((firstMove ?? -1) * 1000) moved_x=\(native.frame.origin.x - origin.x)")
            }
            print("NATIVE_MOVE_RESULT count=\(starts.count) max_ms=\((starts.max() ?? -1) * 1000) request_max_ms=\((moveLatencies.max() ?? -1) * 1000)")
            XCTAssertEqual(starts.count, 3)
            if let maximum = starts.max() { XCTAssertLessThan(maximum, 0.5) }
            return
        }
        if env["NATIVEPIPE_TEST_MOVE"] == "1" {
            for _ in 0..<8 {
                let x = native.contentView!.bounds.midX
                XCTAssertTrue(display.bridge.computerPointerMove(window: window.id, x: x, y: 18))
                XCTAssertTrue(display.bridge.computerPointerButton(window: window.id, button: .left, pressed: true))
                try await Task.sleep(for: .milliseconds(30))
                moveSent = ProcessInfo.processInfo.systemUptime
                XCTAssertTrue(display.bridge.computerPointerMove(window: window.id, x: x + 40, y: 18))
                let deadline = Date().addingTimeInterval(3)
                while moveSent != nil && Date() < deadline { try await Task.sleep(for: .milliseconds(5)) }
                XCTAssertNil(moveSent, "GTK must request a title-bar move for this held drag")
                XCTAssertTrue(display.bridge.computerPointerButton(window: window.id, button: .left, pressed: false))
                try await Task.sleep(for: .milliseconds(150))
            }
            moveLatencies.sort()
            print("REMOTE_MOVE_REQUEST count=\(moveLatencies.count) p50_ms=\((moveLatencies.dropFirst(moveLatencies.count / 2).first ?? -1) * 1000) max_ms=\((moveLatencies.last ?? -1) * 1000)")
            XCTAssertEqual(moveLatencies.count, 8)
            if let maximum = moveLatencies.last { XCTAssertLessThan(maximum, 0.5) }
            return
        }
        statistics.reset(); decoded = 0; presented = 0
        actuallyDisplayed = 0; discarded = 0
        let start = ProcessInfo.processInfo.systemUptime
        var roundtrips: [Double] = []
        let samples = max(20, Int(env["NATIVEPIPE_TEST_ROUNDTRIPS"] ?? "20") ?? 20)
        var intervalStart = start, intervalDisplayed = 0
        for index in 0..<samples {
            let sent = ProcessInfo.processInfo.systemUptime
            // An invalid desktop ID traverses both control directions and the
            // application's service thread without launching another process.
            _ = try? await display.session.applicationClient.launch("nativepipe-latency-probe-not-installed.desktop")
            roundtrips.append(ProcessInfo.processInfo.systemUptime - sent)
            try await Task.sleep(for: .milliseconds(300))
            if (index + 1) % 10 == 0 {
                let now = ProcessInfo.processInfo.systemUptime
                print("REMOTE_INTERVAL elapsed=\(now - start) displayed_fps=\(Double(actuallyDisplayed - intervalDisplayed) / (now - intervalStart))")
                intervalStart = now; intervalDisplayed = actuallyDisplayed
            }
        }
        let seconds = ProcessInfo.processInfo.systemUptime - start
        print("REMOTE_DISPLAY_INTERVAL nanoseconds=\(displayInterval)")
        roundtrips.sort()
        let drift = arrivals.last.map { last in arrivals.first.map { first in
            (last.0 - first.0) - (last.1 - first.1)
        } ?? 0 } ?? 0
        print("REMOTE_LATENCY seconds=\(seconds) video_frames=\(videoFrames) decoded=\(decoded) presentation_acks=\(presented) displayed=\(actuallyDisplayed) discarded=\(discarded) video_bytes=\(videoBytes) alpha_bytes=\(alphaBytes) rtt_p50_ms=\(roundtrips[samples / 2] * 1000) rtt_p95_ms=\(roundtrips[(samples * 95 - 1) / 100] * 1000) rtt_max_ms=\(roundtrips[samples - 1] * 1000) arrival_age_growth_ms=\(drift * 1000)")
        XCTAssertGreaterThan(decoded, 5)
        XCTAssertGreaterThan(Double(actuallyDisplayed) / seconds, 10, "Visible animation must sustain over 10 actual presentations/s")
        if env["NATIVEPIPE_TEST_INPUT"] == "1" {
            for click in 1...6 {
                inputTarget = click
                inputSent = ProcessInfo.processInfo.systemUptime
                XCTAssertTrue(display.bridge.computerPointerMove(window: window.id,
                    x: native.contentView!.bounds.midX, y: native.contentView!.bounds.midY))
                XCTAssertTrue(display.bridge.computerPointerButton(window: window.id, button: .left, pressed: true))
                XCTAssertTrue(display.bridge.computerPointerButton(window: window.id, button: .left, pressed: false))
                let deadline = Date().addingTimeInterval(3)
                while inputSent != nil && Date() < deadline {
                    try await Task.sleep(for: .milliseconds(5))
                }
                XCTAssertNil(inputSent, "The actually presented scene must contain this click's unique pixel marker")
            }
            XCTAssertEqual(inputLatencies.count, 6)
            if inputLatencies.count == 6 {
                inputLatencies.sort()
                print("REMOTE_INPUT_TO_PRESENTED p50_ms=\(inputLatencies[3] * 1000) max_ms=\(inputLatencies.last! * 1000)")
                XCTAssertLessThan(inputLatencies.last!, 0.5, "Input must change the presented frame within 500 ms")
            }
        }
        if env["NATIVEPIPE_TEST_RESIZE"] == "1" {
            for size in [CGSize(width: 1091, height: 721), CGSize(width: 713, height: 489), CGSize(width: 877, height: 613)] {
                let previous = imageSize, before = actuallyDisplayed
                // Exercise AppKit's final-configure path, not a bare geometry
                // change which a still-committed client geometry may replace.
                native.delegate?.windowWillStartLiveResize?(Notification(name: NSWindow.willStartLiveResizeNotification, object: native))
                native.setContentSize(size)
                native.delegate?.windowDidEndLiveResize?(Notification(name: NSWindow.didEndLiveResizeNotification, object: native))
                let resized = Date().addingTimeInterval(8)
                while (imageSize == previous || actuallyDisplayed <= before + 1 || native.contentView!.bounds.size != size) && Date() < resized {
                    try await Task.sleep(for: .milliseconds(50))
                }
                XCTAssertNotEqual(imageSize, previous, "A resize must produce a new-size encoded image")
                XCTAssertGreaterThan(actuallyDisplayed, before + 1)
                XCTAssertEqual(native.contentView!.bounds.size, size, "GTK must accept the requested logical size")
                print("REMOTE_RESIZE content=\(native.contentView!.bounds.size) image=\(imageSize)")
            }
        }
        if let output = env["NATIVEPIPE_TEST_CAPTURE"] {
            let capture = try await display.bridge.captureDockWindow(window.id, maximumWidth: 1200, maximumHeight: 900)
            try capture.png.write(to: URL(fileURLWithPath: output))
        }
    }
    private static func centerMarker(_ surface: IOSurfaceRef) -> Int {
        IOSurfaceLock(surface, .readOnly, nil)
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        let offset = IOSurfaceGetHeight(surface) / 2 * IOSurfaceGetBytesPerRow(surface) + IOSurfaceGetWidth(surface) / 2 * 4
        let bytes = IOSurfaceGetBaseAddress(surface).advanced(by: offset).assumingMemoryBound(to: UInt8.self)
        guard abs(Int(bytes[0]) - Int(bytes[1])) < 8, abs(Int(bytes[1]) - Int(bytes[2])) < 8 else { return -1 }
        return Int((Double(bytes[0]) * 10 / 255).rounded()) - 1
    }
    func testCatalogBinaryReplyAndLimits() throws {
        var payload = ApplicationReply.magic + Data([1, 1, 0, 0])
        func number(_ value: UInt32) { var v = value.littleEndian; withUnsafeBytes(of: &v) { payload.append(contentsOf: $0) } }
        func field(_ bytes: Data) { number(UInt32(bytes.count)); payload.append(bytes) }
        number(42); number(1)
        for value in ["editor.desktop", "Éditeur", "Text editor", "editor", "Editor", "editor-icon"] { field(Data(value.utf8)) }
        guard case .batch(let token, let apps) = try ApplicationReply.decode(payload) else {
            return XCTFail("Expected catalog")
        }
        XCTAssertEqual(token, 42)
        XCTAssertEqual(apps.first?.name, "Éditeur")
        XCTAssertEqual(apps.first?.iconName, "editor-icon")
        XCTAssertThrowsError(try ApplicationReply.decode(payload.dropLast()))
        XCTAssertThrowsError(try ApplicationReply.decode(payload + Data([0])))
        var command = SSHCommand(destination: "host", application: [])
        XCTAssertThrowsError(try command.validate())
        command.persistentSession = true
        XCTAssertNoThrow(try command.validate())
        XCTAssertTrue(command.remoteScript.contains("--stdio --session"))
    }
    func testCredentialRetryAttemptIsPerSessionAndPrompt() throws {
        let directory = try SSHCredentialStore.makeAttemptDirectory(environment: [:])
        defer { try? FileManager.default.removeItem(at: directory) }
        let attributes = try FileManager.default.attributesOfItem(atPath: directory.path)
        XCTAssertEqual((attributes[.posixPermissions] as? NSNumber)?.intValue, 0o700)
        XCTAssertTrue(SSHCredentialStore.claimCachedAttempt(prompt: "password:", directory: directory.path))
        XCTAssertFalse(SSHCredentialStore.claimCachedAttempt(prompt: "password:", directory: directory.path))
        XCTAssertTrue(SSHCredentialStore.claimCachedAttempt(prompt: "key passphrase:", directory: directory.path))
    }
    func testKeychainRoundTrip() throws {
        guard ProcessInfo.processInfo.environment["NATIVEPIPE_TEST_KEYCHAIN"] == "1" else {
            throw XCTSkip("Opt-in test writes only a disposable test credential")
        }
        let store = SSHCredentialStore(connection: "nativepipe-test-" + UUID().uuidString)
        let prompt = "test password:"
        defer { try? store.remove(prompt) }
        try store.save("first", prompt: prompt)
        XCTAssertEqual(try store.read(prompt), "first")
        try store.save("second", prompt: prompt)
        XCTAssertEqual(try store.read(prompt), "second")
        try store.remove(prompt)
        XCTAssertNil(try store.read(prompt))
    }
    @MainActor func testLiveRemoteImmediateExit() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let destination = env["NATIVEPIPE_TEST_REMOTE"],
              let compositor = env["NATIVEPIPE_TEST_COMPOSITOR"] else {
            throw XCTSkip("Requires an explicitly selected remote test machine")
        }
        for (program, status) in [("false", Int32(1)), ("true", Int32(0))] {
            let session = RemoteSession(command: .init(destination: destination, application: [program],
                sshArguments: ["-o", "BatchMode=yes"], compositor: compositor))
            let connecting = Task { try await session.connect() }
            let timeout = Task { try? await Task.sleep(for: .seconds(10)); connecting.cancel() }
            _ = try? await connecting.value
            timeout.cancel()
            let deadline = Date().addingTimeInterval(5)
            while session.exitStatus == nil && Date() < deadline {
                try await Task.sleep(for: .milliseconds(50))
            }
            XCTAssertEqual(session.exitStatus, status, "The application exit must terminate SSH")
            session.disconnect()
        }
    }
    @MainActor func testLiveRemoteCatalogLaunchAndCapture() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let destination = env["NATIVEPIPE_TEST_REMOTE"],
              let compositor = env["NATIVEPIPE_TEST_COMPOSITOR"] else {
            throw XCTSkip("Requires an explicitly selected remote test machine")
        }
        _ = NSApplication.shared
        var command = SSHCommand(destination: destination, application: [],
                                 sshArguments: ["-o", "BatchMode=yes"], compositor: compositor)
        command.persistentSession = true
        let display = RemoteDisplayController(command: command)
        display.session.onDiagnostic = { fputs($0, stderr) }
        defer { display.disconnect() }
        try await display.connect()
        let applications = try await display.session.applications()
        XCTAssertFalse(applications.isEmpty)
        let demo = try XCTUnwrap(applications.first { $0.executable.contains("gtk4-demo") })
        try await display.session.launchApplication(demo.id)
        let deadline = Date().addingTimeInterval(20)
        while display.bridge.dockWindows.isEmpty && Date() < deadline {
            try await Task.sleep(for: .milliseconds(50))
        }
        let window = try XCTUnwrap(display.bridge.dockWindows.first)
        XCTAssertGreaterThan(window.width, 0)
        XCTAssertTrue(display.bridge.activateDockWindow(window.id))
        try await Task.sleep(for: .seconds(2))
        let capture = try await display.bridge.captureDockWindow(window.id, maximumWidth: 1200, maximumHeight: 900)
        XCTAssertGreaterThan(capture.png.count, 3000)
        if let output = env["NATIVEPIPE_TEST_CAPTURE"] {
            try capture.png.write(to: URL(fileURLWithPath: output))
        }
        XCTAssertTrue(display.bridge.requestCloseDockWindow(window.id))
        let closing = Date().addingTimeInterval(5)
        while !display.bridge.dockWindows.isEmpty && Date() < closing {
            try await Task.sleep(for: .milliseconds(50))
        }
        XCTAssertTrue(display.bridge.dockWindows.isEmpty)
    }
}
