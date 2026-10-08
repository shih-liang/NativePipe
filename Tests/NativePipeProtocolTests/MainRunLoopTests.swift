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
        var finished = false
        defer { finished = true }
        let workerReady = DispatchSemaphore(value: 0)
        let trackingWillWait = DispatchSemaphore(value: 0)
        let enqueued = DispatchSemaphore(value: 0)
        let observer = CFRunLoopObserverCreateWithHandler(nil,
            CFRunLoopActivity.beforeWaiting.rawValue, false, 0) { _, _ in
                trackingWillWait.signal()
            }!
        CFRunLoopAddObserver(CFRunLoopGetMain(), observer, mode)
        defer { CFRunLoopRemoveObserver(CFRunLoopGetMain(), observer, mode) }
        DispatchQueue.global(qos: .userInitiated).async {
            workerReady.signal()
            guard trackingWillWait.wait(timeout: .now() + 3) == .success else { return }
            MainRunLoop.perform {
                guard !finished else { return }
                delivered = true
                CFRunLoopStop(CFRunLoopGetMain())
            }
            enqueued.signal()
        }
        // Prepare the producer before measuring wakeup. Starting it on a
        // delayed global task also measured unrelated runner scheduling load.
        guard workerReady.wait(timeout: .now() + 2) == .success else {
            XCTFail("Background producer did not start")
            return
        }
        let start = ProcessInfo.processInfo.systemUptime
        let result = CFRunLoopRunInMode(mode, 2, false)
        let elapsed = ProcessInfo.processInfo.systemUptime - start
        XCTAssertEqual(enqueued.wait(timeout: .now() + 2), .success)
        XCTAssertTrue(delivered)
        XCTAssertEqual(result, .stopped)
        // A queued block without CFRunLoopWakeUp may run only when the two
        // second tracking wait expires. Keep that regression distinguishable
        // from the prepared producer's normal scheduling jitter.
        XCTAssertLessThan(elapsed, 1)
    }
}
