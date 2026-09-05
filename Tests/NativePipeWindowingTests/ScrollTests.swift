import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class ScrollTests: XCTestCase {
    private final class ScrollEvent: NSEvent {
        var inverted = false
        override var scrollingDeltaX: CGFloat { 2 }
        override var scrollingDeltaY: CGFloat { -3 }
        override var isDirectionInvertedFromDevice: Bool { inverted }
        override var hasPreciseScrollingDeltas: Bool { true }
    }

    func testRealWindowPathUsesLiveScrollPreference() throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        bridge.apply(.surfaceCreated(surface: 1))
        bridge.apply(.toplevelCreated(window: 1, surface: 1))
        let native = try XCTUnwrap(bridge.window(1))
        native.revealToplevel(width: 160, height: 100)
        defer { bridge.closeAll() }
        let view = try XCTUnwrap(native.window?.contentView)
        var deltas: [(Double, Double)] = []
        bridge.output = {
            if case .pointerScroll(_, let x, let y, let precise) = $0 {
                XCTAssertTrue(precise)
                deltas.append((x, y))
            }
        }
        let event = ScrollEvent()
        for inverted in [false, true] {
            event.inverted = inverted
            for natural: Bool? in [nil, true, false] {
                bridge.setIntegrationPreferences(.init(naturalScrolling: natural))
                view.scrollWheel(with: event)
                let actual = try XCTUnwrap(deltas.last)
                let sign: Double = natural.map { $0 == inverted ? 1 : -1 } ?? 1
                XCTAssertEqual(actual.0, -2 * sign)
                XCTAssertEqual(actual.1, 3 * sign)
            }
        }
        XCTAssertEqual(deltas.count, 6)
    }
}
