import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class WindowPresenceTests: XCTestCase {
    func testDockPresenceFollowsOpenToplevelsAcrossUnmapAndRemap() throws {
        let app = NSApplication.shared
        let originalPolicy = app.activationPolicy()
        let bridge = WindowBridge(frameSource: nil)
        defer { bridge.closeAll(); app.setActivationPolicy(originalPolicy) }
        var transitions: [Bool] = []
        bridge.onWindowPresenceChanged = { present in
            transitions.append(present)
            let policy: NSApplication.ActivationPolicy = present ? .regular : .accessory
            if app.activationPolicy() != policy { XCTAssertTrue(app.setActivationPolicy(policy)) }
        }
        XCTAssertEqual(app.activationPolicy(), .accessory)
        for id: UInt32 in [3, 4] {
            bridge.apply(.surfaceCreated(surface: id + 10))
            bridge.apply(.toplevelCreated(window: id, surface: id + 10))
        }
        XCTAssertEqual(transitions, [false], "Metadata without a window must not show a Dock icon")
        let first = try XCTUnwrap(bridge.window(3))
        first.revealToplevel()
        XCTAssertEqual(app.activationPolicy(), .regular)
        first.window?.miniaturize(nil)
        first.window?.orderOut(nil)
        XCTAssertTrue(bridge.hasApplicationWindows, "Hidden and minimized windows must remain recoverable")
        bridge.window(4)?.revealToplevel()
        bridge.apply(.surfaceUnmapped(surface: 13))
        XCTAssertTrue(bridge.hasApplicationWindows, "The second window is still open")
        bridge.apply(.surfaceUnmapped(surface: 14))
        XCTAssertEqual(app.activationPolicy(), .accessory)
        first.revealToplevel()
        XCTAssertEqual(app.activationPolicy(), .regular)
        bridge.closeAll()
        XCTAssertEqual(app.activationPolicy(), .accessory)
        XCTAssertEqual(transitions, [false, true, false, true, false])
    }
}
