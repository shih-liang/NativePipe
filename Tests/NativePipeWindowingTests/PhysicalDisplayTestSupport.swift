import AppKit
import XCTest

/// Positive drawable timestamps need a real presentation destination. An
/// active, unlocked WindowServer (including GitHub's Apple Virtual display)
/// does not establish that prerequisite. This opt-in is test-only: the caller
/// must provide an unlocked physical display and explicitly request the check.
enum PhysicalDisplayTestSupport {
    static let optInEnvironmentKey = "NATIVEPIPE_PHYSICAL_DISPLAY_TESTS"

    @MainActor static func requireVisibleDisplay() throws -> NSScreen {
        if let session = CGSessionCopyCurrentDictionary() as? [String: Any] {
            if session["CGSSessionScreenIsLocked"] as? Bool == true {
                throw XCTSkip("Physical display validation not run: the desktop is locked")
            }
            if session["kCGSSessionOnConsoleKey"] as? Bool == false ||
                session["kCGSessionLoginDoneKey"] as? Bool == false {
                throw XCTSkip("Physical display validation not run: no logged-in console session")
            }
        }
        guard let screen = NSScreen.main,
              let id = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber,
              CGDisplayIsActive(id.uint32Value) != 0 else {
            throw XCTSkip("Physical display validation not run: no active display")
        }
        guard screen.localizedName != "Apple Virtual" else {
            throw XCTSkip("Physical display validation not run: observed Apple Virtual display; its active state cannot prove real drawable presentation")
        }
        guard ProcessInfo.processInfo.environment[optInEnvironmentKey] == "1" else {
            throw XCTSkip("Physical display validation not run: set \(optInEnvironmentKey)=1 on an unlocked physical desktop; observed display=\(screen.localizedName)")
        }
        return screen
    }
}
