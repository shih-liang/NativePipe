import Foundation
import CoreFoundation

/// Deliver UI work in AppKit's normal and tracking modes, and wake an idle
/// event loop. Enqueuing a run-loop block alone does not wake nextEvent().
public enum MainRunLoop {
    public static func perform(_ action: @escaping @MainActor @Sendable () -> Void) {
        let loop = CFRunLoopGetMain()
        let modes = [CFRunLoopMode.commonModes.rawValue, "NSEventTrackingRunLoopMode" as CFString] as CFArray
        CFRunLoopPerformBlock(loop, modes) { MainActor.assumeIsolated { action() } }
        CFRunLoopWakeUp(loop)
    }
}
