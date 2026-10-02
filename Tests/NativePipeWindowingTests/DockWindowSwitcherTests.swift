import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class DockWindowSwitcherTests: XCTestCase {
    func testSingleClickActionUsesMachineLocalIconsAndWindow() throws {
        _ = NSApplication.shared
        let vm = WindowBridge(frameSource: nil), remote = WindowBridge(frameSource: nil)
        let vmIcon = NSImage(size: NSSize(width: 32, height: 32))
        let remoteIcon = NSImage(size: NSSize(width: 32, height: 32))
        vm.applicationIconProvider = { _ in vmIcon }
        remote.applicationIconProvider = { _ in remoteIcon }
        defer { vm.closeAll(); remote.closeAll() }
        for bridge in [vm, remote] {
            for id: UInt32 in [3, 4] {
                _ = try mappedWindow(id, bridge: bridge)
                bridge.apply(.appIDChanged(window: id, appID: "same.application.id"))
            }
        }
        for (bridge, icon) in [(vm, vmIcon), (remote, remoteIcon)] {
            let switcher = DockWindowSwitcherController(title: "Machine", bridge: { bridge })
            let window = try XCTUnwrap(bridge.window(3)?.window)
            window.orderOut(nil)
            switcher.showWindows()
            let cell = try XCTUnwrap(switcher.tableView(switcher.tableView, viewFor: nil, row: 0) as? NSTableCellView)
            XCTAssertTrue(cell.imageView?.image === icon, "Identical app/window IDs must keep each machine's icon")
            switcher.tableView.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
            XCTAssertNotNil(switcher.tableView.action, "A single table click performs the primary action")
            switcher.tableView.sendAction(switcher.tableView.action, to: switcher.tableView.target)
            XCTAssertFalse(switcher.isShowingSwitcher)
            XCTAssertTrue(window.isVisible, "One action restores the window without confirmation")
        }
    }

    func testDirectCloseIconTargetsItsRowInsteadOfKeyboardSelection() throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        defer { bridge.closeAll() }
        _ = try mappedWindow(3, bridge: bridge)
        _ = try mappedWindow(4, bridge: bridge)
        var requested: [UInt32] = []
        bridge.output = { if case .close(let id) = $0 { requested.append(id) } }
        let switcher = DockWindowSwitcherController(title: "VM", bridge: { bridge })
        switcher.showWindows()
        let cell = try XCTUnwrap(switcher.tableView(switcher.tableView, viewFor: nil, row: 0))
        func buttons(in view: NSView) -> [NSButton] {
            (view as? NSButton).map { [$0] } ?? view.subviews.flatMap { buttons(in: $0) }
        }
        let controls = buttons(in: cell)
        XCTAssertEqual(Set(controls.compactMap(\.toolTip)), ["Minimize", "Zoom", "Enter Full Screen", "Close Window"])
        XCTAssertTrue(controls.allSatisfy { $0.image != nil }, "Every visible control must have its icon")
        let close = try XCTUnwrap(controls.first { $0.toolTip == "Close Window" })
        switcher.tableView.selectRowIndexes(IndexSet(integer: 1), byExtendingSelection: false)
        close.performClick(nil)
        XCTAssertEqual(requested, [3])
        XCTAssertFalse(switcher.isShowingSwitcher)
    }

    func testZeroWindowsDoesNotOpenUtilityLauncher() {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        let switcher = DockWindowSwitcherController(title: "VM", bridge: { bridge }, utilities: {
            [.init(title: "Console", symbol: "terminal", visible: false, enabled: true,
                   action: { XCTFail("No launcher action on an empty Dock reopen") })]
        })
        switcher.showWindows()
        XCTAssertFalse(switcher.isShowingSwitcher)
    }

    func testOpenPanelUpdatesAfterCloseMetadataAndSearch() async throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        bridge.applicationNameProvider = { _ in "Text Editor" }
        let switcher = DockWindowSwitcherController(title: "研究 VM", bridge: { bridge })
        defer { bridge.closeAll() }
        _ = try mappedWindow(3, bridge: bridge)
        _ = try mappedWindow(4, bridge: bridge)
        bridge.apply(.titleChanged(window: 3, title: "报告 2026 — final.txt"))
        bridge.apply(.appIDChanged(window: 3, appID: "org.gnome.TextEditor"))
        switcher.showWindows()
        switcher.searchField.stringValue = "报告 editor 研究"
        switcher.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification))
        XCTAssertEqual(switcher.displayedWindowIDs, [3])
        XCTAssertEqual(switcher.selectedWindowID, 3)
        bridge.apply(.titleChanged(window: 3, title: "Different report"))
        try await Task.sleep(for: .milliseconds(30))
        XCTAssertTrue(switcher.displayedWindowIDs.isEmpty)
        XCTAssertNil(switcher.selectedWindowID)
        switcher.cancelOperation(nil)
        XCTAssertEqual(switcher.displayedWindowIDs.count, 2, "Escape clears a search first")
        bridge.apply(.surfaceUnmapped(surface: 13))
        try await Task.sleep(for: .milliseconds(30))
        XCTAssertEqual(switcher.displayedWindowIDs, [4])
        XCTAssertTrue(switcher.isShowingSwitcher, "Do not activate a different window after an external close")
        bridge.apply(.surfaceUnmapped(surface: 14))
        try await Task.sleep(for: .milliseconds(30))
        XCTAssertFalse(switcher.isShowingSwitcher)
    }

    func testSelectionCannotCrossHostsOrReusedWindowIDs() throws {
        _ = NSApplication.shared
        let vm = WindowBridge(frameSource: nil), remote = WindowBridge(frameSource: nil)
        var current = vm
        let switcher = DockWindowSwitcherController(title: "VM", bridge: { current })
        defer { vm.closeAll(); remote.closeAll() }
        _ = try mappedWindow(3, bridge: vm)
        _ = try mappedWindow(4, bridge: vm)
        let remoteWindow = try mappedWindow(3, bridge: remote)
        remoteWindow.orderOut(nil)
        switcher.showWindows()
        current = remote
        switcher.showSelectedWindow()
        XCTAssertFalse(remoteWindow.isVisible, "A stale VM action must not activate remote ID 3")
        XCTAssertFalse(switcher.isShowingSwitcher)
        current = vm
        switcher.showWindows()
        let selected = try XCTUnwrap(switcher.selectedWindowID)
        vm.apply(.toplevelDestroyed(window: selected))
        let replacement = try mappedWindow(selected, bridge: vm)
        replacement.orderOut(nil)
        switcher.showSelectedWindow()
        XCTAssertFalse(replacement.isVisible, "Even an ID reused inside one bridge is a new target")
    }

    func testKeyboardSelectionAfterReopeningActivatesCurrentRow() throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        let switcher = DockWindowSwitcherController(title: "VM", bridge: { bridge })
        defer { bridge.closeAll() }
        let first = try mappedWindow(3, bridge: bridge)
        let second = try mappedWindow(4, bridge: bridge)
        bridge.apply(.titleChanged(window: 3, title: "A report"))
        bridge.apply(.titleChanged(window: 4, title: "B visible report"))
        switcher.showWindows()
        switcher.cancelOperation(nil)
        switcher.showWindows()
        switcher.tableView.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
        XCTAssertEqual(switcher.selectedWindowID, 3)
        switcher.moveSelection(1)
        XCTAssertEqual(switcher.selectedWindowID, 4)
        switcher.moveSelection(-1)
        XCTAssertEqual(switcher.selectedWindowID, 3)
        first.orderOut(nil); second.orderOut(nil)
        switcher.showSelectedWindow()
        XCTAssertTrue(first.isVisible)
        XCTAssertFalse(second.isVisible)
    }

    func testKeyboardSelectionRestoresCorrectWindowAndInputResponder() async throws {
        let app = NSApplication.shared
        let originalPolicy = app.activationPolicy()
        let bridge = WindowBridge(frameSource: nil)
        bridge.onWindowPresenceChanged = { app.setActivationPolicy($0 ? .regular : .accessory) }
        let switcher = DockWindowSwitcherController(title: "Linux", bridge: { bridge })
        defer { bridge.closeAll(); app.setActivationPolicy(originalPolicy) }
        let first = try mappedWindow(3, bridge: bridge)
        let second = try mappedWindow(4, bridge: bridge)
        first.miniaturize(nil)
        switcher.showWindows()
        try await Task.sleep(for: .milliseconds(60))
        // XCTest processes aren't always granted foreground activation. The
        // standalone preview verifies the OS key-window transition as an app.
        guard app.isActive, switcher.isShowingSwitcher else {
            throw XCTSkip("Needs a foreground AppKit application; also covered by the standalone preview")
        }
        XCTAssertTrue(app.keyWindow === switcher.searchField.window)
        XCTAssertFalse(first.isKeyWindow)
        switcher.tableView.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
        switcher.moveSelection(1)
        XCTAssertEqual(switcher.selectedWindowID, 4)
        switcher.moveSelection(-1)
        switcher.showSelectedWindow()
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertFalse(switcher.isShowingSwitcher)
        XCTAssertFalse(first.isMiniaturized)
        if app.isActive { XCTAssertTrue(first.isKeyWindow) }
        XCTAssertTrue(first.firstResponder === first.contentView)
        XCTAssertFalse(second.isKeyWindow)
    }

    func testHostSubtitleDoesNotChangeGuestTitleOrContentGeometry() throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        defer { bridge.closeAll() }
        bridge.machineName = "VM A"
        let window = try mappedWindow(3, bridge: bridge)
        let native = try XCTUnwrap(bridge.window(3))
        bridge.apply(.titleChanged(window: 3, title: "Guest title"))
        native.setServerDecorated(true)
        let size = window.contentView?.bounds.size
        XCTAssertEqual(window.subtitle, "VM A")
        bridge.machineName = "VM B"
        XCTAssertEqual(window.title, "Guest title")
        XCTAssertEqual(window.subtitle, "VM B")
        XCTAssertEqual(window.contentView?.bounds.size, size)
        native.setServerDecorated(false)
        XCTAssertEqual(window.subtitle, "", "CSD must not acquire an extra titlebar")
    }

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
