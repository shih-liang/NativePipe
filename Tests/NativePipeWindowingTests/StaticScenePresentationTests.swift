import AppKit
import IOSurface
import Metal
import NativePipeProtocol
import QuartzCore
import XCTest
@testable import NativePipeWindowing

@MainActor
private final class StaticSceneSource: FrameSource {
    var textures: [UInt32: MTLTexture] = [:]
    func surface(forResource resourceID: UInt32) -> IOSurfaceRef? { nil }
    func metalTexture(forResource resourceID: UInt32, width: Int, height: Int,
        bytesPerRow: Int, format: UInt32) -> AnyObject? { textures[resourceID] }
}

final class StaticScenePresentationTests: XCTestCase {
    /// A static client sends only one Wayland commit. Subsequent private
    /// publications below stand in for captureFrame's new protected buffer
    /// leases; each result itself comes from a real WindowServer drawable.
    @MainActor func testStaticRootSceneRetriesKnownZeroAfterAcknowledgementUntilActuallyDisplayed() async throws {
        _ = NSApplication.shared
        let previousPolicy = NSApp.activationPolicy()
        NSApp.setActivationPolicy(.accessory)
        NSApp.finishLaunching()
        defer { NSApp.setActivationPolicy(previousPolicy) }
        try requireVisibleDesktop()
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let source = StaticSceneSource()
        let bridge = WindowBridge(frameSource: source)
        defer { bridge.output = nil; bridge.closeAll() }
        var nextID: UInt32 = 0
        var zeros: [UInt32] = []
        var positives: [(UInt32, UInt64)] = []
        var acknowledged: Set<UInt32> = []
        var released: [UInt32] = []
        var protocolFailure: Error?
        var refreshes = 0
        let width = 320, height = 240

        func publishProtectedSnapshot() throws {
            guard nextID < 5 else { throw POSIXError(.ELOOP) }
            nextID += 1
            let id = nextID
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                width: width, height: height, mipmapped: false)
            descriptor.storageMode = .shared
            descriptor.usage = [.shaderRead]
            let texture = try XCTUnwrap(device.makeTexture(descriptor: descriptor))
            let pixels: [UInt8] = Array(repeating: [UInt8(48), 120, 240, 255], count: width * height).flatMap { $0 }
            pixels.withUnsafeBytes {
                texture.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0,
                    withBytes: $0.baseAddress!, bytesPerRow: width * 4)
            }
            source.textures[id] = texture
            let layer = Windowing.SceneLayer(surface: 97, resourceID: id,
                width: width, height: height, bytesPerRow: width * 4, format: .bgra8888,
                destination: .init(x: 0, y: 0, width: Double(width), height: Double(height)),
                sourcePixels: .init(x: 0, y: 0, width: Double(width), height: Double(height)),
                clip: .init(x: 0, y: 0, width: Double(width), height: Double(height)), opaque: true)
            let now = UInt64((CACurrentMediaTime() * 1_000_000_000).rounded())
            bridge.apply(.sceneCommitted(scene: .init(surface: 97, presentationID: id,
                width: width, height: height, scale: 1,
                windowGeometry: .init(x: 0, y: 0, width: width, height: height), layers: [layer],
                presentationContext: .init(sessionID: 7, clockEpoch: 1, guestSendTimeNanoseconds: 100 + UInt64(id)),
                receivedHostTimeNanoseconds: now)))
        }

        bridge.output = { command in
            switch command {
            case .frameReleased(97, let id):
                released.append(id)
                source.textures[id] = nil
            case .presentationFeedback(7, 1, 97, let id, let time, _, _):
                if time == 0 { zeros.append(id) } else { positives.append((id, time)) }
                // Model a distinct guest/display receipt rather than reentering
                // the sender while it is still enqueueing terminal feedback.
                Task { @MainActor in
                    acknowledged.insert(id)
                    bridge.apply(.presentationFeedbackAcknowledged(sessionID: 7, clockEpoch: 1,
                        surface: 97, presentationID: id))
                }
            case .captureFrame(surface: 97):
                XCTAssertTrue(zeros.allSatisfy { acknowledged.contains($0) },
                    "A new attempt must follow every preceding zero receipt across independent lanes")
                refreshes += 1
                do { try publishProtectedSnapshot() } catch { protocolFailure = error }
            default: break
            }
        }
        bridge.apply(.surfaceCreated(surface: 97))
        bridge.apply(.toplevelCreated(window: 8, surface: 97))
        let before = UInt64((CACurrentMediaTime() * 1_000_000_000).rounded())
        try publishProtectedSnapshot()
        // A command-line XCTest process need not become the foreground app.
        // Use a real visible window above existing apps for the display check.
        bridge.window(8)?.window?.level = .popUpMenu
        bridge.window(8)?.window?.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        bridge.window(8)?.window?.orderFrontRegardless()
        let deadline = ProcessInfo.processInfo.systemUptime + 6
        while positives.isEmpty, protocolFailure == nil, ProcessInfo.processInfo.systemUptime < deadline {
            for _ in 0..<100 {
                guard let event = NSApp.nextEvent(matching: .any, until: .distantPast,
                    inMode: .default, dequeue: true) else { break }
                NSApp.sendEvent(event)
            }
            try await Task.sleep(for: .milliseconds(5))
        }
        try requireVisibleDesktop()
        if let protocolFailure { throw protocolFailure }
        let native = try XCTUnwrap(bridge.window(8))
        let displayed = try XCTUnwrap(positives.first,
            "A static initial commit must eventually reach a real display; zeros=\(zeros), refreshes=\(refreshes), policy=\(NSApp.activationPolicy()), visible=\(native.window?.isVisible == true), occlusion=\(String(describing: native.window?.occlusionState)), frame=\(String(describing: native.window?.frame)), screen=\(String(describing: native.window?.screen))")
        XCTAssertGreaterThan(displayed.1, before)
        XCTAssertEqual(refreshes, Int(nextID - 1))
        XCTAssertEqual(Set(zeros).count, zeros.count, "Initial zero facts are kept and acknowledged exactly once")
        for _ in 0..<100 {
            if released.count == Int(nextID) { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertEqual(released.sorted(), Array(UInt32(1)...nextID))
        XCTAssertLessThanOrEqual(nextID, 5)
    }

    @MainActor private func requireVisibleDesktop() throws {
        if let session = CGSessionCopyCurrentDictionary() as? [String: Any],
           session["CGSSessionScreenIsLocked"] as? Bool == true {
            throw XCTSkip("Desktop locked; a real display result cannot be verified")
        }
        guard let screen = NSScreen.main,
              let id = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber,
              CGDisplayIsActive(id.uint32Value) != 0 else { throw XCTSkip("No active display") }
    }
}
