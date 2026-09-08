import Foundation
import NativePipeProtocol
import XCTest

final class ScrollCoalescingTests: XCTestCase {
    private func scroll(_ x: Double = 0, _ y: Double = 0,
                        window: UInt32 = 1, precise: Bool = true) -> Windowing.HostCommand {
        .pointerScroll(window: window, dx: x, dy: y, isPrecise: precise)
    }

    func testAdjacentSameDirectionMotionCoalescesWithoutChangingDistance() throws {
        let merged = try XCTUnwrap(scroll(2, -3).coalescingScroll(with: scroll(4, -5)))
        XCTAssertEqual(try WindowWire.commandPayload(for: merged),
                       try WindowWire.commandPayload(for: scroll(6, -8)))
    }

    func testStopRemainsAnOrderedBarrierInEitherPosition() {
        XCTAssertNil(scroll(0, 3).coalescingScroll(with: scroll()))
        XCTAssertNil(scroll().coalescingScroll(with: scroll(0, 3)))
        XCTAssertNil(scroll().coalescingScroll(with: scroll()))
    }

    func testReversalCannotCancelIntoFalseStopOrHideAChangeOfDirection() {
        XCTAssertNil(scroll(0, 3).coalescingScroll(with: scroll(0, -3)))
        XCTAssertNil(scroll(2, 3).coalescingScroll(with: scroll(-1, 4)))
        XCTAssertNil(scroll(2, 3).coalescingScroll(with: scroll(1, -1)))
    }

    func testDifferentWindowsOrSourcesDoNotMerge() {
        XCTAssertNil(scroll(0, 3).coalescingScroll(with: scroll(0, 3, window: 2)))
        XCTAssertNil(scroll(0, 3).coalescingScroll(with: scroll(0, 3, precise: false)))
        XCTAssertNil(scroll(0, 3).coalescingScroll(with: .pointerLeft(window: 1)))
    }

    func testInvalidOrOverflowingDisplacementsDoNotMerge() {
        XCTAssertNil(scroll(0, .infinity).coalescingScroll(with: scroll(0, 3)))
        XCTAssertNil(scroll(0, 3).coalescingScroll(with: scroll(.nan, 0)))
        XCTAssertNil(scroll(0, .greatestFiniteMagnitude).coalescingScroll(
            with: scroll(0, .greatestFiniteMagnitude)))
    }

    func testStopKeepsExistingGuestWireContract() throws {
        let payload = try WindowWire.commandPayload(for: scroll())
        XCTAssertEqual(payload.count, 28)
        XCTAssertEqual(Array(payload.prefix(8)), [78, 80, 83, 67, 1, 0, 0, 0])
        XCTAssertEqual(Array(payload[8..<24]), Array(repeating: 0, count: 16))
        XCTAssertEqual(Array(payload[24..<28]), [1, 0, 0, 0])
    }
}
