import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class HostIntegrationTests: XCTestCase {
    func testVMHostResolutionIsSharedAndImmediatelyApplied() {
        _ = NSApplication.shared
        let integration = HostIntegrationController()
        var desktop: [DesktopPreferences.ColorScheme] = []
        var windows: [WindowIntegrationPreferences] = []
        integration.applyDesktop = { desktop.append($0.colorScheme) }
        integration.applyWindows = { windows.append($0) }
        var preferences = WindowIntegrationPreferences(keyboardLayout: "us")
        preferences.naturalScrolling = false
        integration.update(windows: preferences, appearance: .dark)
        XCTAssertEqual(desktop.last, .dark)
        XCTAssertEqual(windows.last?.keyboardLayout, "us")
        XCTAssertEqual(windows.last?.naturalScrolling, false)
        preferences.keyboardLayout = ""
        preferences.naturalScrolling = nil
        integration.update(windows: preferences, appearance: .light)
        XCTAssertEqual(desktop.last, .light)
        XCTAssertEqual(windows.last?.keyboardLayout, HostKeyboardLayout.current())
        XCTAssertNil(windows.last?.naturalScrolling)
        integration.sync() // The same path is replayed when either backend connects.
        XCTAssertEqual(desktop, [.dark, .light, .light])
        XCTAssertEqual(windows.count, 3)
    }
}
