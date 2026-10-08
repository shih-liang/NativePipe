import AppKit
import IOSurface
import Metal
import NativePipeProtocol
import XCTest
@testable import NativePipeWindowing

@MainActor
private final class AuxiliaryBridgeSource: FrameSource {
    var texture: MTLTexture?
    var resolutions = 0
    func surface(forResource resourceID: UInt32) -> IOSurfaceRef? { nil }
    func metalTexture(forResource resourceID: UInt32, width: Int, height: Int,
        bytesPerRow: Int, format: UInt32) -> AnyObject? { resolutions += 1; return texture }
}

final class AuxiliaryBridgeTests: XCTestCase {
    private func frame(_ id: UInt32, queried: Bool = true) -> Windowing.Frame {
        .init(resourceID: id, width: 48, height: 24, bytesPerRow: 192, format: .bgra8888,
            scale: 2, presentationID: id,
            presentationContext: .init(sessionID: 7, clockEpoch: 3, guestSendTimeNanoseconds: 100 + UInt64(id)),
            receivedHostTimeNanoseconds: 200 + UInt64(id), requestsPresentationFeedback: queried)
    }

    private func results(_ commands: [Windowing.HostCommand]) -> [(UInt32, UInt32, UInt64)] {
        commands.compactMap {
            if case .presentationFeedback(7, 3, let surface, let id, let time, _, _) = $0 {
                return (surface, id, time)
            }
            return nil
        }
    }

    private func released(_ commands: [Windowing.HostCommand]) -> [UInt32] {
        commands.compactMap {
            if case .frameReleased(97, let id) = $0 { return id }
            return nil
        }
    }

    @MainActor func testUnroledQueryKeepsFullIdentityAndReaderClockUntilSupersededOrDestroyed() {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        defer { bridge.closeAll() }
        bridge.apply(.surfaceCreated(surface: 97))
        bridge.apply(.committed(surface: 97, frame: frame(19)))
        XCTAssertTrue(commands.contains {
            if case .sceneClockSample(7, 3, 97, 19, 119, 219) = $0 { return true }; return false
        })
        XCTAssertTrue(released(commands).isEmpty)
        bridge.apply(.committed(surface: 97, frame: frame(20)))
        bridge.apply(.presentationClockRequested(token: 1, sessionID: 7, clockEpoch: 3))
        XCTAssertEqual(released(commands), [19])
        XCTAssertEqual(results(commands).map { $0.0 }, [97])
        XCTAssertEqual(results(commands).map { $0.1 }, [19])
        XCTAssertEqual(results(commands).map { $0.2 }, [0])
        bridge.apply(.surfaceDestroyed(surface: 97))
        bridge.apply(.presentationClockRequested(token: 2, sessionID: 7, clockEpoch: 3))
        XCTAssertEqual(released(commands), [19, 20])
        XCTAssertEqual(results(commands).map { $0.1 }, [19, 20])
    }

    @MainActor func testHiddenQueriedCursorDoesNotInventDisplayAndSemanticShapeRetiresItsWaitingRead() {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        defer { bridge.closeAll() }
        bridge.apply(.surfaceCreated(surface: 97))
        bridge.apply(.cursorChanged(surface: 97, hotspotX: 3, hotspotY: 2))
        bridge.apply(.committed(surface: 97, frame: frame(19)))
        XCTAssertTrue(released(commands).isEmpty)
        XCTAssertTrue(results(commands).isEmpty)
        bridge.apply(.cursorShapeChanged(shape: .text))
        bridge.apply(.presentationClockRequested(token: 1, sessionID: 7, clockEpoch: 3))
        XCTAssertTrue(bridge.currentPointerCursor() === NSCursor.iBeam)
        XCTAssertEqual(released(commands), [19])
        XCTAssertEqual(results(commands).map { $0.2 }, [0])
    }

    @MainActor func testDeferredResourceAndHiddenRoleShareOneLatestProtectedPublication() {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: AuxiliaryBridgeSource())
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        defer { bridge.closeAll() }
        bridge.apply(.surfaceCreated(surface: 97))
        bridge.apply(.cursorChanged(surface: 97, hotspotX: 0, hotspotY: 0))
        bridge.apply(.committed(surface: 97, frame: frame(19, queried: false))) // resource-deferred
        bridge.apply(.committed(surface: 97, frame: frame(20))) // hidden, waiting for pointer enter
        XCTAssertEqual(released(commands), [19])
        bridge.apply(.committed(surface: 97, frame: frame(21, queried: false))) // resource-deferred again
        bridge.apply(.presentationClockRequested(token: 1, sessionID: 7, clockEpoch: 3))
        XCTAssertEqual(released(commands), [19, 20])
        XCTAssertEqual(results(commands).map { $0.1 }, [20])
        bridge.retryPendingFrames()
        XCTAssertEqual(released(commands), [19, 20], "Retry must not revive either obsolete publication")
        bridge.apply(.cursorChanged(surface: nil, hotspotX: 0, hotspotY: 0))
        XCTAssertEqual(released(commands), [19, 20, 21])
    }

    @MainActor func testUnqueriedCustomCursorUsesNSCursorAndNoPresentationLedger() throws {
        _ = NSApplication.shared
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let source = AuxiliaryBridgeSource()
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
            width: 48, height: 24, mipmapped: false)
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderRead]
        let texture = try XCTUnwrap(device.makeTexture(descriptor: descriptor))
        source.texture = texture
        let pixels = [UInt8](repeating: 255, count: 48 * 24 * 4)
        pixels.withUnsafeBytes {
            source.texture?.replace(region: MTLRegionMake2D(0, 0, 48, 24), mipmapLevel: 0,
                withBytes: $0.baseAddress!, bytesPerRow: 192)
        }
        let bridge = WindowBridge(frameSource: source)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        defer { bridge.closeAll() }
        bridge.apply(.surfaceCreated(surface: 97))
        bridge.apply(.cursorChanged(surface: 97, hotspotX: 3, hotspotY: 2))
        bridge.apply(.committed(surface: 97, frame: frame(19, queried: false)))
        XCTAssertEqual(bridge.currentPointerCursor().image.size, CGSize(width: 24, height: 12))
        XCTAssertEqual(bridge.currentPointerCursor().hotSpot, CGPoint(x: 3, y: 2))
        XCTAssertEqual(released(commands), [19])
        XCTAssertFalse(commands.contains { if case .sceneClockSample = $0 { return true }; return false })
        let rewrittenPixels = [UInt8](repeating: 0, count: 48 * 24 * 4)
        rewrittenPixels.withUnsafeBytes {
            texture.replace(region: MTLRegionMake2D(0, 0, 48, 24), mipmapLevel: 0,
                withBytes: $0.baseAddress!, bytesPerRow: 192)
        }
        let cursorImage = try XCTUnwrap(bridge.currentPointerCursor().image.cgImage(
            forProposedRect: nil, context: nil, hints: nil))
        let cursorPixels = try XCTUnwrap(cursorImage.dataProvider?.data) as Data
        XCTAssertEqual(Array(cursorPixels.prefix(4)), [255, 255, 255, 255],
            "NSCursor owns an eager pixel copy before frameReleased permits a guest rewrite")
        source.texture = nil // The source hold is already released; only host pixels remain.
        bridge.apply(.cursorChanged(surface: 97, hotspotX: 6, hotspotY: 3, pixelScale: 2))
        XCTAssertEqual(bridge.currentPointerCursor().image.size, CGSize(width: 16, height: 8))
        XCTAssertEqual(bridge.currentPointerCursor().hotSpot, CGPoint(x: 4, y: 2))
        XCTAssertEqual(source.resolutions, 1, "Hotspot/scale changes must never sample a released guest texture")
        XCTAssertEqual(released(commands), [19])
        bridge.apply(.presentationClockRequested(token: 1, sessionID: 7, clockEpoch: 3))
        XCTAssertTrue(results(commands).isEmpty, "Copy completion cannot claim NSCursor display time")
    }

    @MainActor func testExportSuppressionPersistsForLateDragCommitsUntilGuestEndsGesture() {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        defer { bridge.closeAll() }
        bridge.apply(.surfaceCreated(surface: 97))
        bridge.apply(.dragIconChanged(surface: 97))
        bridge.hideDragIcon()
        bridge.apply(.committed(surface: 97, frame: frame(19)))
        bridge.apply(.committed(surface: 97, frame: frame(20)))
        XCTAssertEqual(released(commands), [19, 20], "The Mac drag owns its host file icons; late guest images stay suppressed")
        bridge.apply(.dragIconChanged(surface: nil))
        bridge.apply(.dragIconChanged(surface: 97))
        bridge.apply(.committed(surface: 97, frame: frame(21)))
        XCTAssertEqual(released(commands), [19, 20], "The next guest gesture can wait for its own display normally")
    }

    @MainActor func testZeroAttemptFencesRefreshAndCloseRemovesQueuedRefreshBeforeSameNonceReplay() {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        defer { bridge.closeAll() }
        bridge.apply(.surfaceCreated(surface: 97))
        bridge.apply(.committed(surface: 97, frame: frame(19)))
        bridge.apply(.committed(surface: 97, frame: frame(20)))
        bridge.apply(.presentationClockRequested(token: 1, sessionID: 7, clockEpoch: 3))
        commands.removeAll()
        bridge.send(.captureFrame(surface: 97))
        XCTAssertFalse(commands.contains { if case .captureFrame = $0 { return true }; return false })
        bridge.apply(.presentationFeedbackAcknowledged(sessionID: 7, clockEpoch: 3, surface: 97, presentationID: 99))
        XCTAssertFalse(commands.contains { if case .captureFrame = $0 { return true }; return false }, "An unrelated acknowledgement cannot release the refresh")
        bridge.closeAll()
        commands.removeAll()
        bridge.apply(.channelReady(sessionID: 22, protocolVersion: WindowWire.windowProtocolVersion))
        bridge.apply(.presentationClockRequested(token: 2, sessionID: 7, clockEpoch: 4))
        XCTAssertEqual(results(commands).map { $0.1 }, [19, 20])
        bridge.apply(.presentationFeedbackAcknowledged(sessionID: 7, clockEpoch: 3, surface: 97, presentationID: 19))
        bridge.apply(.presentationFeedbackAcknowledged(sessionID: 7, clockEpoch: 3, surface: 97, presentationID: 20))
        XCTAssertFalse(commands.contains { if case .captureFrame = $0 { return true }; return false }, "The closed gesture cannot refresh a reused surface id")
    }

    @MainActor func testPauseCancelsPendingAuxiliaryQueryBeforeDrain() async throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { command in
            commands.append(command)
            switch command {
            case .presentationPause(let session, let token):
                bridge.apply(.presentationPauseReached(sessionID: session, token: token))
            case .presentationDrain(let session, let token):
                XCTAssertEqual(Set(self.results(commands).map { $0.1 }), [19])
                XCTAssertEqual(self.released(commands), [19])
                bridge.apply(.presentationDrained(sessionID: session, token: token))
            default: break
            }
        }
        defer { bridge.output = nil; bridge.closeAll() }
        bridge.apply(.surfaceCreated(surface: 97))
        bridge.apply(.committed(surface: 97, frame: frame(19)))
        try await bridge.prepareForVirtualMachinePause()
        XCTAssertFalse(bridge.acceptsKeyboardInput)
    }

    @MainActor func testMatchingDiscardAcknowledgementReleasesExactlyOneLiveExplicitCapture() async throws {
        _ = NSApplication.shared
        guard MTLCreateSystemDefaultDevice() != nil else { throw XCTSkip("Metal unavailable") }
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        defer { bridge.output = nil; bridge.closeAll() }
        bridge.apply(.surfaceCreated(surface: 97))
        bridge.apply(.committed(surface: 97, frame: frame(19)))
        bridge.apply(.toplevelCreated(window: 8, surface: 97))
        let native = try XCTUnwrap(bridge.window(8))
        let emptyScene = Windowing.SceneSnapshot(surface: 97, presentationID: 0,
            width: 48, height: 24, scale: 1,
            windowGeometry: .init(x: 0, y: 0, width: 48, height: 24), layers: [])
        XCTAssertTrue(native.present(scene: emptyScene, layers: [], latchIDs: [], readComplete: { _ in }))
        native.window?.orderOut(nil)
        native.reportWindowState()
        commands.removeAll()
        let capture = Task { @MainActor in try await native.captureFrame() }
        for _ in 0..<100 {
            if native.hasPendingFrameCapture { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(native.hasPendingFrameCapture)
        XCTAssertFalse(commands.contains { if case .captureFrame = $0 { return true }; return false })
        bridge.apply(.presentationFeedbackAcknowledged(sessionID: 7, clockEpoch: 3, surface: 97, presentationID: 99))
        XCTAssertFalse(commands.contains { if case .captureFrame = $0 { return true }; return false })
        bridge.apply(.presentationFeedbackAcknowledged(sessionID: 7, clockEpoch: 3, surface: 97, presentationID: 19))
        bridge.apply(.presentationFeedbackAcknowledged(sessionID: 7, clockEpoch: 3, surface: 97, presentationID: 19))
        XCTAssertEqual(commands.filter { if case .captureFrame(surface: 97) = $0 { return true }; return false }.count, 1,
            "Explicit capture may be occluded, but must wait for the real feedback acknowledgement")
        bridge.closeAll()
        do { _ = try await capture.value; XCTFail("No guest capture scene was supplied") }
        catch { XCTAssertTrue(error is ComputerUseWindowError) }
    }

    @MainActor func testDiscardAcknowledgedWhileHiddenRetainsRefreshUntilWindowReturns() async throws {
        _ = NSApplication.shared
        let previousPolicy = NSApp.activationPolicy()
        NSApp.setActivationPolicy(.accessory)
        NSApp.finishLaunching()
        defer { NSApp.setActivationPolicy(previousPolicy) }
        if let session = CGSessionCopyCurrentDictionary() as? [String: Any],
           session["CGSSessionScreenIsLocked"] as? Bool == true {
            throw XCTSkip("Desktop locked; window visibility cannot be verified")
        }
        guard MTLCreateSystemDefaultDevice() != nil else { throw XCTSkip("Metal unavailable") }
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        defer { bridge.output = nil; bridge.closeAll() }
        bridge.apply(.surfaceCreated(surface: 97))
        bridge.apply(.committed(surface: 97, frame: frame(19)))
        bridge.apply(.toplevelCreated(window: 8, surface: 97))
        let native = try XCTUnwrap(bridge.window(8))
        XCTAssertTrue(native.present(scene: .init(surface: 97, presentationID: 0,
            width: 320, height: 240, scale: 1,
            windowGeometry: .init(x: 0, y: 0, width: 320, height: 240), layers: []),
            layers: [], latchIDs: [], readComplete: { _ in }))
        native.window?.orderOut(nil)
        native.reportWindowState()
        commands.removeAll()
        bridge.send(.captureFrame(surface: 97))
        bridge.apply(.presentationFeedbackAcknowledged(sessionID: 7, clockEpoch: 3,
            surface: 97, presentationID: 19))
        XCTAssertFalse(commands.contains { if case .captureFrame = $0 { return true }; return false })
        // Keep this real test window above the user's foreground apps without
        // relying on XCTest's process becoming the frontmost application.
        native.window?.level = .popUpMenu
        native.window?.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        native.window?.orderFrontRegardless()
        let deadline = ProcessInfo.processInfo.systemUptime + 2
        while !native.canPresent, ProcessInfo.processInfo.systemUptime < deadline {
            for _ in 0..<100 {
                guard let event = NSApp.nextEvent(matching: .any, until: .distantPast,
                    inMode: .default, dequeue: true) else { break }
                NSApp.sendEvent(event)
            }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(native.canPresent,
            "policy=\(NSApp.activationPolicy()) visible=\(native.window?.isVisible == true) occlusion=\(String(describing: native.window?.occlusionState)) frame=\(String(describing: native.window?.frame)) screen=\(String(describing: native.window?.screen))")
        native.reportWindowState()
        native.reportWindowState()
        bridge.flushDeferredPresentationRefresh(surface: 97)
        XCTAssertEqual(commands.filter { if case .captureFrame(surface: 97) = $0 { return true }; return false }.count, 1,
            "A hidden ACK preserves the intent; restoring visibility consumes it exactly once")
    }

    @MainActor func testPauseResumeRefreshesEachQueriedAuxiliarySurfaceOnceAfterZeroAcknowledgement() async throws {
        _ = NSApplication.shared
        if let session = CGSessionCopyCurrentDictionary() as? [String: Any],
           session["CGSSessionScreenIsLocked"] as? Bool == true { throw XCTSkip("Desktop locked") }
        guard MTLCreateSystemDefaultDevice() != nil else { throw XCTSkip("Metal unavailable") }
        let previousPolicy = NSApp.activationPolicy()
        NSApp.setActivationPolicy(.accessory)
        NSApp.finishLaunching()
        defer { NSApp.setActivationPolicy(previousPolicy) }
        let bridge = WindowBridge(frameSource: AuxiliaryBridgeSource())
        var commands: [Windowing.HostCommand] = []
        bridge.output = { command in
            commands.append(command)
            switch command {
            case .presentationPause(let session, let token):
                bridge.apply(.presentationPauseReached(sessionID: session, token: token))
            case .presentationDrain(let session, let token):
                for surface: UInt32 in [97, 98] {
                    bridge.send(.captureFrame(surface: surface))
                    bridge.apply(.presentationFeedbackAcknowledged(sessionID: 7, clockEpoch: 3,
                        surface: surface, presentationID: 19))
                }
                bridge.apply(.presentationDrained(sessionID: session, token: token))
            case .presentationResume(let session, let token):
                bridge.apply(.presentationResumed(sessionID: session, clockEpoch: 4, token: token))
            default: break
            }
        }
        defer { bridge.output = nil; bridge.closeAll() }
        bridge.apply(.surfaceCreated(surface: 1))
        bridge.apply(.toplevelCreated(window: 8, surface: 1))
        let native = try XCTUnwrap(bridge.window(8))
        XCTAssertTrue(native.present(scene: .init(surface: 1, presentationID: 0,
            width: 320, height: 240, scale: 1,
            windowGeometry: .init(x: 0, y: 0, width: 320, height: 240), layers: []),
            layers: [], latchIDs: [], readComplete: { _ in }))
        native.window?.level = .popUpMenu
        native.window?.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        native.window?.orderFrontRegardless()
        NSApp.activate(ignoringOtherApps: true)
        let deadline = ProcessInfo.processInfo.systemUptime + 2
        while !(native.canPresent && NSApp.isActive), ProcessInfo.processInfo.systemUptime < deadline {
            for _ in 0..<100 {
                guard let event = NSApp.nextEvent(matching: .any, until: .distantPast,
                    inMode: .default, dequeue: true) else { break }
                NSApp.sendEvent(event)
            }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(native.canPresent && NSApp.isActive)
        bridge.pointerPresentationEntered(window: 8, position: NSEvent.mouseLocation)
        for surface: UInt32 in [97, 98] {
            bridge.apply(.surfaceCreated(surface: surface))
            bridge.apply(.committed(surface: surface, frame: frame(19)))
        }
        bridge.apply(.cursorChanged(surface: 97, hotspotX: 0, hotspotY: 0))
        bridge.apply(.dragIconChanged(surface: 98))
        commands.removeAll()
        try await bridge.prepareForVirtualMachinePause()
        XCTAssertFalse(commands.contains { if case .captureFrame = $0 { return true }; return false },
            "Acknowledgements cannot publish auxiliary content while the pause gate is closed")
        try await bridge.resumePresentationAfterVirtualMachinePause()
        for surface: UInt32 in [97, 98] {
            XCTAssertEqual(commands.filter { if case .captureFrame(let id) = $0 { return id == surface }; return false }.count, 1,
                "The gate and auxiliary refresh must consume one shared intent for surface \(surface)")
        }
    }
}
