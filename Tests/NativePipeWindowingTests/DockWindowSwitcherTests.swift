import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class DockWindowSwitcherTests: XCTestCase {
    func testSingleWindowRestoresWithoutSwitcherAndIgnoresUnmappedObjects() throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        let switcher = DockWindowSwitcherController(title: "Linux", bridge: { bridge })
        defer { bridge.closeAll() }
        let window = try mappedWindow(3, bridge: bridge)
        bridge.apply(.surfaceCreated(surface: 14))
        bridge.apply(.toplevelCreated(window: 4, surface: 14))
        bridge.apply(.surfaceCreated(surface: 15))
        bridge.apply(.popupCreated(window: 5, surface: 15, parent: 3,
                                  x: 10, y: 10, width: 100, height: 100))
        window.miniaturize(nil)
        window.orderOut(nil)

        switcher.showWindows()

        XCTAssertFalse(switcher.isShowingSwitcher)
        XCTAssertTrue(window.isVisible)
        XCTAssertFalse(window.isMiniaturized)
        XCTAssertTrue(window.firstResponder === window.contentView)
        XCTAssertEqual(bridge.dockWindows.map(\.id), [3])
    }

    func testMultipleWindowsChooseButOneRemainingWindowClosesStaleSwitcher() throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        let switcher = DockWindowSwitcherController(title: "Linux", bridge: { bridge })
        defer { bridge.closeAll() }
        let first = try mappedWindow(3, bridge: bridge)
        let second = try mappedWindow(4, bridge: bridge)
        second.miniaturize(nil)
        switcher.showWindows()
        XCTAssertTrue(switcher.isShowingSwitcher, "A minimized window is still a choice")

        bridge.apply(.surfaceUnmapped(surface: 14))
        first.orderOut(nil)
        switcher.showWindows()

        XCTAssertFalse(switcher.isShowingSwitcher)
        XCTAssertTrue(first.isVisible)
    }

    func testOnlyOpenUtilityWindowsParticipateInChoosing() throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        var screenOpen = false
        var screenActivations = 0
        let switcher = DockWindowSwitcherController(title: "VM", bridge: { bridge }, utilities: {
            [.init(title: "Console", symbol: "terminal", visible: false, enabled: true,
                   action: { XCTFail("An unopened console is not a window") }),
             .init(title: "Screen", symbol: "display", visible: screenOpen, enabled: true,
                   action: { screenActivations += 1 })]
        })
        defer { bridge.closeAll() }
        _ = try mappedWindow(3, bridge: bridge)
        switcher.showWindows()
        XCTAssertFalse(switcher.isShowingSwitcher)
        XCTAssertEqual(screenActivations, 0)

        screenOpen = true
        switcher.showWindows()
        XCTAssertTrue(switcher.isShowingSwitcher, "A guest window plus Screen needs a choice")
        bridge.closeAll()
        switcher.showWindows()
        XCTAssertFalse(switcher.isShowingSwitcher)
        XCTAssertEqual(screenActivations, 1, "Screen alone activates directly")
    }

    private func mappedWindow(_ id: UInt32, bridge: WindowBridge) throws -> NSWindow {
        bridge.apply(.surfaceCreated(surface: id + 10))
        bridge.apply(.toplevelCreated(window: id, surface: id + 10))
        let native = try XCTUnwrap(bridge.window(id))
        native.revealToplevel()
        return try XCTUnwrap(native.window)
    }
}
