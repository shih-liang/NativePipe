import CoreFoundation
import XCTest
@testable import NativePipeProtocol

final class MainRunLoopTests: XCTestCase {
    @MainActor func testBackgroundDeliveryWakesIdleTrackingLoop() {
        let mode = CFRunLoopMode(rawValue: "NSEventTrackingRunLoopMode" as CFString)
        // Keep this mode alive without a timer or dispatch-main wakeup.
        var context = CFRunLoopSourceContext()
        let source = CFRunLoopSourceCreate(nil, 0, &context)!
        CFRunLoopAddSource(CFRunLoopGetMain(), source, mode)
        defer { CFRunLoopRemoveSource(CFRunLoopGetMain(), source, mode) }
        var delivered = false
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.02) {
            MainRunLoop.perform { delivered = true; CFRunLoopStop(CFRunLoopGetMain()) }
        }
        let start = Date()
        CFRunLoopRunInMode(mode, 1, false)
        XCTAssertTrue(delivered)
        XCTAssertLessThan(Date().timeIntervalSince(start), 0.3)
    }
}
