import AppKit
@testable import NativePipeWindowing
import XCTest

@MainActor
final class WindowActivationTests: XCTestCase {
    private func window() -> NSWindow {
        _ = NSApplication.shared
        return NSWindow(contentRect: NSRect(x: 0, y: 0, width: 100, height: 100),
                 styleMask: [.titled], backing: .buffered, defer: true)
    }

    func testARecentHostPressAuthorizesExactlyOneActivation() {
        let authority = WindowActivationAuthority(), origin = window()
        authority.record(window: origin, id: 5, now: 100)
        XCTAssertTrue(authority.consume(window: origin, id: 5, guestInputAgeMilliseconds: 50,
                                        appIsActive: true, originIsKey: true, now: 100.05))
        XCTAssertFalse(authority.consume(window: origin, id: 5, guestInputAgeMilliseconds: 50,
                                         appIsActive: true, originIsKey: true, now: 100.05))
    }

    func testGuestAgeCannotSubstituteForRealHostInputOrCurrentFocus() {
        let authority = WindowActivationAuthority(), origin = window()
        XCTAssertFalse(authority.consume(window: origin, id: 5, guestInputAgeMilliseconds: 0,
                                         appIsActive: true, originIsKey: true, now: 100))
        for (active, key, time, guestAge) in [
            (false, true, 100.1, UInt32(100)), (true, false, 100.1, 100),
            (true, true, 105.001, 0), (true, true, 99.0, 0),
            (true, true, 100.1, 5_001), (true, true, Double.nan, 0)
        ] {
            authority.record(window: origin, id: 5, now: 100)
            XCTAssertFalse(authority.consume(window: origin, id: 5, guestInputAgeMilliseconds: guestAge,
                                             appIsActive: active, originIsKey: key, now: time))
        }
    }

    func testWindowIdentityAndConnectionResetPreventAuthorityReuse() {
        let authority = WindowActivationAuthority(), first = window(), replacement = window()
        authority.record(window: first, id: 5, now: 100)
        XCTAssertFalse(authority.consume(window: replacement, id: 5, guestInputAgeMilliseconds: 0,
                                         appIsActive: true, originIsKey: true, now: 100.1))
        authority.record(window: first, id: 5, now: 100)
        XCTAssertFalse(authority.consume(window: first, id: 6, guestInputAgeMilliseconds: 0,
                                         appIsActive: true, originIsKey: true, now: 100.1))
        authority.record(window: first, id: 5, now: 100)
        authority.clear()
        XCTAssertFalse(authority.consume(window: first, id: 5, guestInputAgeMilliseconds: 0,
                                         appIsActive: true, originIsKey: true, now: 100.1))
    }
}
