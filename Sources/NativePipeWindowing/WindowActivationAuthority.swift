import AppKit

/// Guest activation also needs authority from a press actually sent by this
/// host. A guest-provided age alone cannot establish recent user interaction.
@MainActor
final class WindowActivationAuthority {
    private weak var origin: NSWindow?
    private var originID: UInt32?
    private var inputTime: TimeInterval = 0

    func record(window: NSWindow, id: UInt32, now: TimeInterval = ProcessInfo.processInfo.systemUptime) {
        origin = window
        originID = id
        inputTime = now
    }

    func clear() {
        origin = nil
        originID = nil
    }

    func consume(
        window: NSWindow, id: UInt32, guestInputAgeMilliseconds: UInt32,
        appIsActive: Bool, originIsKey: Bool,
        now: TimeInterval = ProcessInfo.processInfo.systemUptime
    ) -> Bool {
        defer { clear() }
        return appIsActive && originIsKey && origin === window && originID == id &&
            guestInputAgeMilliseconds <= 5_000 && now.isFinite && inputTime.isFinite &&
            now >= inputTime && now - inputTime <= 5
    }
}
