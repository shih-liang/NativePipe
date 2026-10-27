import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

/// The agent's focused window is configured as activated alongside the macOS
/// key window, at once rather than on the next display tick.
@MainActor
final class AgentFocusTests: XCTestCase {
    func testAgentFocusActivatesItsWindowOnly() throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        defer { bridge.closeAll() }
        for window: UInt32 in [1, 2] {
            bridge.apply(.surfaceCreated(surface: window + 10))
            bridge.apply(.toplevelCreated(window: window, surface: window + 10))
            try XCTUnwrap(bridge.window(window)).revealToplevel(width: 160, height: 100)
        }
        var states: [UInt32: [Windowing.ToplevelState]] = [:]
        bridge.output = {
            if case .configure(let window, _, let configured, _) = $0 { states[window] = configured }
        }
        bridge.apply(.agentFocusChanged(window: 2))
        XCTAssertEqual(bridge.agentFocusWindow, 2)
        XCTAssertEqual(states[2]?.contains(.activated), true)
        XCTAssertNil(states[1], "only the agent's window changes")

        bridge.apply(.agentFocusChanged(window: 1))
        XCTAssertEqual(states[1]?.contains(.activated), true)
        XCTAssertEqual(states[2]?.contains(.activated), false)
        bridge.apply(.agentFocusChanged(window: nil))
        XCTAssertNil(bridge.agentFocusWindow)
        XCTAssertEqual(states[1]?.contains(.activated), false)
    }
}
