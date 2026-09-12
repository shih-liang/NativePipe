import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class WindowMoveTests: XCTestCase {
    func testMoveUsesOriginalDownAndReleasesGuestGrabWithoutMouseUp() throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        let native = NativeWindow(windowID: 3, surfaceID: 8, bridge: bridge)
        native.revealToplevel()
        defer { native.close() }
        let window = try XCTUnwrap(native.window)
        let view = try XCTUnwrap(window.contentView)
        var buttons: [Bool] = []
        bridge.output = { if case .pointerButton(_, .left, let pressed) = $0 { buttons.append(pressed) } }
        let down = try event(.leftMouseDown, window: window, x: 200)
        view.mouseDown(with: down)
        view.mouseDragged(with: try event(.leftMouseDragged, window: window, x: 300))
        XCTAssertTrue(native.takeWindowMoveEvent(pressedMouseButtons: 1) === down)
        XCTAssertNil(native.takeWindowMoveEvent(pressedMouseButtons: 1), "One request can hand off only once")
        native.finishWindowMoveIfReleased(pressedMouseButtons: 1)
        XCTAssertEqual(buttons, [true])
        native.finishWindowMoveIfReleased(pressedMouseButtons: 0)
        view.mouseUp(with: try event(.leftMouseUp, window: window, x: 300))
        XCTAssertEqual(buttons, [true, false], "A missing or late AppKit up must release exactly once")
    }

    func testReleasedOrUnrelatedPressCannotStartWindowMove() throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        let native = NativeWindow(windowID: 3, surfaceID: 8, bridge: bridge)
        native.revealToplevel()
        defer { native.close() }
        let window = try XCTUnwrap(native.window)
        let down = try event(.leftMouseDown, window: window, x: 200)
        native.pointerButton(.left, pressed: true, event: down)
        XCTAssertNil(native.takeWindowMoveEvent(pressedMouseButtons: 0), "A reply after physical release is stale")
        XCTAssertNil(native.takeWindowMoveEvent(pressedMouseButtons: 1))
        native.pointerButton(.left, pressed: true, event: down)
        native.pointerButton(.left, pressed: false)
        XCTAssertNil(native.takeWindowMoveEvent(pressedMouseButtons: 1))
        native.pointerButton(.left, pressed: true)
        XCTAssertNil(native.takeWindowMoveEvent(pressedMouseButtons: 1), "Protocol-only input cannot borrow NSApp.currentEvent")
        native.pointerButton(.left, pressed: true, event: down)
        native.close()
        XCTAssertNil(native.takeWindowMoveEvent(pressedMouseButtons: 1))
    }

    private func event(_ type: NSEvent.EventType, window: NSWindow, x: CGFloat) throws -> NSEvent {
        try XCTUnwrap(NSEvent.mouseEvent(with: type, location: CGPoint(x: x, y: 580),
            modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
            windowNumber: window.windowNumber, context: nil, eventNumber: 1,
            clickCount: 1, pressure: type == .leftMouseUp ? 0 : 1))
    }
}
