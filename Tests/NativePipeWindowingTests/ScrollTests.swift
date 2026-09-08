import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class ScrollTests: XCTestCase {
    private final class ScrollEvent: NSEvent {
        var inverted = false
        var dx: CGFloat = 2
        var dy: CGFloat = -3
        var precise = true
        var gesturePhase: NSEvent.Phase = []
        var inertiaPhase: NSEvent.Phase = []
        override var scrollingDeltaX: CGFloat { dx }
        override var scrollingDeltaY: CGFloat { dy }
        override var isDirectionInvertedFromDevice: Bool { inverted }
        override var hasPreciseScrollingDeltas: Bool { precise }
        override var phase: NSEvent.Phase { gesturePhase }
        override var momentumPhase: NSEvent.Phase { inertiaPhase }
    }

    private struct Scroll: Equatable {
        var x: Double
        var y: Double
        var precise = true
        static let stop = Scroll(x: 0, y: 0)
    }

    @MainActor
    private final class Harness {
        let bridge: WindowBridge
        let native: NativeWindow
        let view: NSView
        var scrolls: [Scroll] = []
        var commands: [Windowing.HostCommand] = []

        init() throws {
            _ = NSApplication.shared
            bridge = WindowBridge(frameSource: nil)
            bridge.apply(.surfaceCreated(surface: 1))
            bridge.apply(.toplevelCreated(window: 1, surface: 1))
            native = try XCTUnwrap(bridge.window(1))
            native.revealToplevel(width: 160, height: 100)
            view = try XCTUnwrap(native.window?.contentView)
            bridge.output = { [weak self] command in
                self?.commands.append(command)
                if case .pointerScroll(_, let x, let y, let precise) = command {
                    self?.scrolls.append(Scroll(x: x, y: y, precise: precise))
                }
            }
        }

        func send(_ phase: NSEvent.Phase = [], momentum: NSEvent.Phase = [],
                  dx: CGFloat = 0, dy: CGFloat = 0, precise: Bool = true) {
            let event = ScrollEvent()
            event.gesturePhase = phase
            event.inertiaPhase = momentum
            event.dx = dx
            event.dy = dy
            event.precise = precise
            view.scrollWheel(with: event)
        }
    }

    func testOnlyRealGestureEndProducesStop() throws {
        let h = try Harness()
        defer { h.bridge.closeAll() }
        h.send(.mayBegin)
        h.send(.began)
        h.send(.changed, dx: 4, dy: -12)
        h.send(.stationary)
        h.send(.changed)
        XCTAssertEqual(h.scrolls, [.init(x: -4, y: 12)])
        h.send(.ended)
        h.send(.ended)
        XCTAssertEqual(h.scrolls, [.init(x: -4, y: 12), .stop])
    }

    func testFinalDisplacementPrecedesStop() throws {
        let h = try Harness()
        defer { h.bridge.closeAll() }
        h.send(.began, dy: -8)
        h.send(.ended, dx: 2, dy: -3)
        XCTAssertEqual(h.scrolls, [.init(x: 0, y: 8), .init(x: -2, y: 3), .stop])
    }

    func testCancellationStopsWithoutApplyingCancelledDisplacement() throws {
        let h = try Harness()
        defer { h.bridge.closeAll() }
        h.send(.began, dy: -8)
        h.send(.cancelled, dy: -20)
        h.send(.ended)
        XCTAssertEqual(h.scrolls, [.init(x: 0, y: 8), .stop])
    }

    func testMomentumDoesNotCreateAnotherGestureOrMovePointer() throws {
        let h = try Harness()
        defer { h.bridge.closeAll() }
        h.send(.began, dy: -8)
        h.send(.ended)
        let count = h.commands.count
        h.send(momentum: .began, dy: -30)
        h.send(momentum: .changed, dy: -20)
        h.send(momentum: .ended)
        XCTAssertEqual(h.commands.count, count)
        XCTAssertEqual(h.scrolls, [.init(x: 0, y: 8), .stop])
        h.send(.began, dy: 5)
        h.send(.ended)
        XCTAssertEqual(h.scrolls, [.init(x: 0, y: 8), .stop, .init(x: 0, y: -5), .stop])
    }

    func testMomentumTransitionClosesAnUnterminatedPreciseSequenceOnce() throws {
        let h = try Harness()
        defer { h.bridge.closeAll() }
        h.send(dy: -8)
        h.send(momentum: .began, dy: -30)
        h.send(momentum: .changed, dy: -20)
        h.send(momentum: .ended)
        XCTAssertEqual(h.scrolls, [.init(x: 0, y: 8), .stop])
    }

    func testLegacyWheelKeepsItsDisplacementAndSource() throws {
        let h = try Harness()
        defer { h.bridge.closeAll() }
        h.send(dx: 2, dy: -3, precise: false)
        h.send(precise: false)
        h.send(dy: -1, precise: false)
        XCTAssertEqual(h.scrolls, [.init(x: -2, y: 3, precise: false),
                                   .init(x: 0, y: 1, precise: false)])
    }

    func testPointerLeaveStopsBeforeLosingPointerFocus() throws {
        let h = try Harness()
        defer { h.bridge.closeAll() }
        h.send(.began, dy: -8)
        h.native.pointerLeft()
        XCTAssertEqual(h.scrolls, [.init(x: 0, y: 8), .stop])
        guard case .pointerLeft = h.commands.last else { return XCTFail("missing pointer leave") }
        h.send(.ended)
        XCTAssertEqual(h.scrolls.count, 2)
    }

    func testSuspendAndApplicationDeactivationTerminateGesture() throws {
        let h = try Harness()
        defer { h.bridge.closeAll() }
        h.send(.began, dy: -8)
        h.bridge.setSuspended(true)
        XCTAssertEqual(h.scrolls, [.init(x: 0, y: 8), .stop])
        h.send(.changed, dy: -8)
        XCTAssertEqual(h.scrolls.count, 2)
        h.bridge.setSuspended(false)
        h.send(.began, dy: -3)
        NotificationCenter.default.post(name: NSApplication.didResignActiveNotification,
                                        object: NSApplication.shared)
        XCTAssertEqual(h.scrolls, [.init(x: 0, y: 8), .stop, .init(x: 0, y: 3), .stop])
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
