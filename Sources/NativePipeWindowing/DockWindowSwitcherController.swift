import NativePipeStrings
import AppKit

@MainActor
private final class SwitcherContentController: NSViewController {
    var cancel: (() -> Void)?
    override func cancelOperation(_ sender: Any?) { cancel?() }
}

@MainActor
private final class SwitcherActionButton: NSButton {
    var perform: (() -> Void)?
    @objc func invoke() { perform?() }
}

@MainActor
private final class SwitcherCell: NSTableCellView {
    weak var firstActionButton: NSButton?
}

@MainActor
private final class SwitcherTableView: NSTableView {
    var beginSearch: ((NSEvent?) -> Void)?
    var activateSelection: (() -> Void)?
    var showActions: (() -> Void)?
    override func keyDown(with event: NSEvent) {
        if event.charactersIgnoringModifiers == "\r" || event.charactersIgnoringModifiers == "\u{3}" {
            if event.modifierFlags.contains(.option) { showActions?() }
            else { activateSelection?() }
            return
        }
        if event.specialKey == nil,
           event.modifierFlags.intersection([.command, .control]).isEmpty,
           !(event.charactersIgnoringModifiers ?? "").isEmpty {
            beginSearch?(event)
        } else { super.keyDown(with: event) }
    }
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if event.modifierFlags.intersection([.command, .control, .option]) == .command,
           event.charactersIgnoringModifiers?.lowercased() == "f" {
            beginSearch?(nil)
            return true
        }
        return super.performKeyEquivalent(with: event)
    }
}

/// Reusable UI code, with a separate panel instance for each machine.
/// Every instance stays bound to one host's bridge;
/// neither window IDs nor actions are looked up in a process-global registry.
@MainActor
public final class DockWindowSwitcherController: NSObject, NSPopoverDelegate,
    NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate {
    public struct Utility {
        public let title: String
        public let symbol: String
        public let visible: Bool
        public let enabled: Bool
        public let action: () -> Void

        public init(title: String, symbol: String, visible: Bool, enabled: Bool,
                    action: @escaping () -> Void) {
            self.title = title; self.symbol = symbol
            self.visible = visible; self.enabled = enabled; self.action = action
        }
    }

    private struct Entry {
        let id: String
        let title: String
        let subtitle: String
        let icon: NSImage?
        let window: DockWindow?
        let nativeID: ObjectIdentifier?
        let utility: Utility?
    }
    private final class MenuAction: NSObject {
        let perform: () -> Void
        init(_ perform: @escaping () -> Void) { self.perform = perform }
    }

    public var title: String { didSet { refresh() } }
    private let bridgeProvider: () -> WindowBridge?
    private let utilitiesProvider: () -> [Utility]
    private weak var presentedBridge: WindowBridge?
    private var presentedSession: UUID?
    private var entries: [Entry] = []
    private var filtered: [Entry] = []
    private var observing = false
    private var refreshPending = false
    private let anchorView = NSView(frame: NSRect(x: 0, y: 0, width: 2, height: 2))
    private let anchorPanel: NSPanel
    private let popover = NSPopover()
    private let content = SwitcherContentController()
    private let heading = NSTextField(wrappingLabelWithString: "")
    let searchField = NSSearchField()
    let tableView: NSTableView = SwitcherTableView()
    private let emptyLabel = NSTextField(wrappingLabelWithString: NPText("No matching windows. Try another title or app name."))
    private let actionsButton = NSPopUpButton(frame: .zero, pullsDown: true)
    private let nativeStateNotifications: [Notification.Name] = [
        NSWindow.didMiniaturizeNotification, NSWindow.didDeminiaturizeNotification,
        NSWindow.didEnterFullScreenNotification, NSWindow.didExitFullScreenNotification,
    ]

    public init(title: String, bridge: @escaping () -> WindowBridge?,
                utilities: @escaping () -> [Utility] = { [] }) {
        self.title = title; bridgeProvider = bridge; utilitiesProvider = utilities
        anchorPanel = NSPanel(contentRect: anchorView.frame,
                              styleMask: [.borderless, .nonactivatingPanel],
                              backing: .buffered, defer: false)
        super.init()
        anchorPanel.contentView = anchorView
        anchorPanel.backgroundColor = .clear
        anchorPanel.isOpaque = false
        anchorPanel.hasShadow = false
        anchorPanel.hidesOnDeactivate = false
        anchorPanel.ignoresMouseEvents = true
        anchorPanel.isExcludedFromWindowsMenu = true
        anchorPanel.level = .popUpMenu
        anchorPanel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        popover.behavior = .transient
        popover.delegate = self
        popover.contentViewController = content
        content.cancel = { [weak self] in self?.cancelOperation(nil) }
        buildView()
    }

    var isShowingSwitcher: Bool { popover.isShown }
    var displayedWindowIDs: [UInt32] { filtered.compactMap { $0.window?.id } }
    var selectedWindowID: UInt32? { selectedEntry?.window?.id }
    private var selectedEntry: Entry? {
        filtered.indices.contains(tableView.selectedRow) ? filtered[tableView.selectedRow] : nil
    }

    public func showWindows() {
        // Dock clicks must not bury a connection error or a confirmation dialog.
        if let modal = NSApp.modalWindow {
            activateHost()
            modal.makeKeyAndOrderFront(nil)
            return
        }
        let bridge = bridgeProvider()
        let windows = bridge?.dockWindows ?? []
        let utilities = utilitiesProvider().filter(\.visible)
        let count = windows.count + utilities.count
        guard count > 0 else { closeSwitcher(); return }
        if count == 1 {
            closeSwitcher()
            if let window = windows.first { _ = bridge?.activateDockWindow(window.id) }
            else if let utility = utilities.first, utility.enabled {
                activateHost()
                utility.action()
            }
            return
        }
        if popover.isShown { closeSwitcher(); return }
        presentedBridge = bridge
        presentedSession = bridge?.computerSessionID
        searchField.stringValue = ""
        reloadEntries()
        applyFilter(preferredID: entries.first(where: { $0.window?.isKey == true })?.id)
        startObserving()
        let edge = positionAnchor()
        popover.animates = !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        activateHost()
        anchorPanel.orderFrontRegardless()
        popover.show(relativeTo: anchorView.bounds, of: anchorView, preferredEdge: edge)
    }

    /// Hosts call this when their open utility windows change. Guest window
    /// changes arrive through bridge-scoped notifications, never a polling loop.
    public func refresh() {
        guard popover.isShown else { return }
        guard bridgeProvider() === presentedBridge,
              presentedBridge?.computerSessionID == presentedSession else {
            closeSwitcher(); return
        }
        let selected = selectedEntry?.id
        reloadEntries()
        guard !entries.isEmpty else { closeSwitcher(); return }
        applyFilter(preferredID: selected)
    }

    private func reloadEntries() {
        heading.stringValue = title
        heading.toolTip = title
        heading.setAccessibilityLabel(NPText("Machine: %@", String(describing: (title))))
        searchField.setAccessibilityLabel(NPText("Search open windows on %@", String(describing: (title))))
        tableView.setAccessibilityLabel(NPText("Open windows on %@", String(describing: (title))))
        let bridge = presentedBridge
        entries = (bridge?.dockWindows ?? []).map { window in
            let app = window.applicationName ?? window.applicationID ?? NPText("Application")
            let state = window.isMiniaturized ? NPText("Minimized") : (window.isFullscreen ? NPText("Full Screen") : "")
            return Entry(id: "window:\(window.id)", title: window.title, subtitle: state.isEmpty ? app : NPText("%@ · %@", app, state),
                         icon: bridge?.iconForDockWindow(window.id), window: window,
                         nativeID: bridge?.window(window.id).map(ObjectIdentifier.init), utility: nil)
        }
        entries += utilitiesProvider().filter(\.visible).map {
            Entry(id: "utility:\($0.title)", title: $0.title, subtitle: title,
                  icon: NSImage(systemSymbolName: $0.symbol, accessibilityDescription: nil),
                  window: nil, nativeID: nil, utility: $0)
        }
    }

    private func applyFilter(preferredID: String?) {
        let terms = searchField.stringValue.split(whereSeparator: \.isWhitespace).map(String.init)
        filtered = entries.filter { entry in
            let text = [title, entry.title, entry.subtitle, entry.window?.applicationID ?? ""].joined(separator: " ")
            return terms.allSatisfy { text.localizedStandardContains($0) }
        }
        tableView.reloadData()
        let index = filtered.firstIndex { $0.id == preferredID } ?? (filtered.isEmpty ? nil : 0)
        if let index { tableView.selectRowIndexes(IndexSet(integer: index), byExtendingSelection: false) }
        else { tableView.deselectAll(nil) }
        emptyLabel.isHidden = !filtered.isEmpty
        updateTools()
    }

    private func buildView() {
        let root = NSView()
        content.view = root
        heading.font = .systemFont(ofSize: 14, weight: .semibold)
        heading.maximumNumberOfLines = 2
        heading.setAccessibilityLabel(NPText("Machine"))
        searchField.placeholderString = NPText("Type to search open windows")
        searchField.toolTip = NPText("Type to search, or press ⌘F")
        searchField.setAccessibilityLabel(NPText("Search open windows on this machine"))
        searchField.delegate = self
        searchField.sendsSearchStringImmediately = true
        searchField.sendsWholeSearchString = false
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("window"))
        tableView.addTableColumn(column)
        tableView.headerView = nil
        tableView.rowHeight = 76
        tableView.intercellSpacing = NSSize(width: 0, height: 2)
        tableView.style = .plain
        tableView.backgroundColor = .clear
        tableView.allowsEmptySelection = true
        tableView.dataSource = self; tableView.delegate = self
        tableView.target = self; tableView.action = #selector(showSelectedWindow)
        (tableView as? SwitcherTableView)?.activateSelection = { [weak self] in self?.showSelectedWindow() }
        (tableView as? SwitcherTableView)?.showActions = { [weak self] in self?.focusSelectedActions() }
        (tableView as? SwitcherTableView)?.beginSearch = { [weak self] event in
            guard let self else { return }
            self.searchField.window?.makeFirstResponder(self.searchField)
            if let event { self.searchField.currentEditor()?.interpretKeyEvents([event]) }
        }
        tableView.setAccessibilityLabel(NPText("Open windows on this machine"))
        let scroll = NSScrollView()
        scroll.documentView = tableView
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.borderType = .noBorder
        emptyLabel.textColor = .secondaryLabelColor
        emptyLabel.alignment = .center
        actionsButton.bezelStyle = .rounded
        actionsButton.setAccessibilityLabel(NPText("Machine tools"))
        actionsButton.toolTip = NPText("Open a machine tool")
        let keyboardHint = NSTextField(labelWithString: NPText("↩ Switch    ⌥↩ Window actions"))
        keyboardHint.font = .systemFont(ofSize: 11)
        keyboardHint.textColor = .secondaryLabelColor
        let footer = NSStackView(views: [actionsButton, NSView(), keyboardHint])
        footer.orientation = .horizontal
        footer.spacing = 8
        let separator = NSBox(); separator.boxType = .separator
        for view in [heading, searchField, scroll, emptyLabel, separator, footer] {
            view.translatesAutoresizingMaskIntoConstraints = false; root.addSubview(view)
        }
        NSLayoutConstraint.activate([
            heading.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 16),
            heading.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -16),
            heading.topAnchor.constraint(equalTo: root.topAnchor, constant: 14),
            searchField.topAnchor.constraint(equalTo: heading.bottomAnchor, constant: 12),
            searchField.leadingAnchor.constraint(equalTo: heading.leadingAnchor),
            searchField.trailingAnchor.constraint(equalTo: heading.trailingAnchor),
            scroll.topAnchor.constraint(equalTo: searchField.bottomAnchor, constant: 10),
            scroll.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 8),
            scroll.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -8),
            separator.topAnchor.constraint(equalTo: scroll.bottomAnchor, constant: 8),
            separator.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            separator.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            footer.topAnchor.constraint(equalTo: separator.bottomAnchor, constant: 10),
            footer.leadingAnchor.constraint(equalTo: heading.leadingAnchor),
            footer.trailingAnchor.constraint(equalTo: heading.trailingAnchor),
            footer.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -12),
            emptyLabel.centerYAnchor.constraint(equalTo: scroll.centerYAnchor),
            emptyLabel.leadingAnchor.constraint(equalTo: heading.leadingAnchor),
            emptyLabel.trailingAnchor.constraint(equalTo: heading.trailingAnchor),
        ])
        root.setAccessibilityLabel(NPText("Window switcher"))
        root.nextKeyView = tableView
        searchField.nextKeyView = tableView
        tableView.nextKeyView = actionsButton
        actionsButton.nextKeyView = searchField
    }

    public func numberOfRows(in tableView: NSTableView) -> Int { filtered.count }
    public func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        // AppKit can finish a pending layout after the transient panel closes.
        guard filtered.indices.contains(row) else { return nil }
        let item = filtered[row]
        let cell = SwitcherCell()
        let icon = NSImageView()
        icon.image = item.icon ?? NSImage(systemSymbolName: "macwindow", accessibilityDescription: nil)
        icon.imageScaling = .scaleProportionallyUpOrDown
        let name = NSTextField(wrappingLabelWithString: item.title)
        name.font = .systemFont(ofSize: 13, weight: .medium)
        name.maximumNumberOfLines = 2
        name.lineBreakMode = .byWordWrapping
        let app = NSTextField(labelWithString: item.subtitle)
        app.font = .systemFont(ofSize: 11)
        app.textColor = .secondaryLabelColor
        app.lineBreakMode = .byTruncatingMiddle
        cell.textField = name; cell.imageView = icon
        let buttons = windowActions(for: item)
        cell.firstActionButton = buttons.first
        let metadata = NSStackView(views: [app, NSView()] + buttons)
        metadata.orientation = .horizontal; metadata.spacing = 4
        app.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let text = NSStackView(views: [name, metadata]); text.orientation = .vertical
        text.alignment = .leading; text.spacing = 3
        for view in [icon, text] { view.translatesAutoresizingMaskIntoConstraints = false; cell.addSubview(view) }
        NSLayoutConstraint.activate([
            icon.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 8),
            icon.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
            icon.widthAnchor.constraint(equalToConstant: 48), icon.heightAnchor.constraint(equalToConstant: 48),
            text.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 10),
            text.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -8),
            text.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
            name.widthAnchor.constraint(equalTo: text.widthAnchor),
            metadata.widthAnchor.constraint(equalTo: text.widthAnchor),
        ])
        cell.toolTip = "\(item.title)\n\(item.subtitle)\n\(title)"
        cell.setAccessibilityLabel("\(item.title), \(item.subtitle), \(title)")
        cell.setAccessibilityHelp(NPText("Click to switch to this window. Window controls are available as labeled icon buttons."))
        return cell
    }
    public func controlTextDidChange(_ notification: Notification) { applyFilter(preferredID: selectedEntry?.id) }
    public func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        // Let an IME finish composing before Return/arrows become switch commands.
        guard !textView.hasMarkedText() else { return false }
        switch selector {
        case #selector(NSResponder.moveDown(_:)): moveSelection(1)
        case #selector(NSResponder.moveUp(_:)): moveSelection(-1)
        case #selector(NSResponder.insertNewline(_:)): showSelectedWindow()
        case #selector(NSResponder.cancelOperation(_:)): cancelOperation(nil)
        default: return false
        }
        return true
    }
    func moveSelection(_ delta: Int) {
        guard !filtered.isEmpty else { return }
        let row = min(max(tableView.selectedRow + delta, 0), filtered.count - 1)
        tableView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
        tableView.scrollRowToVisible(row)
    }
    @objc public func cancelOperation(_ sender: Any?) {
        if !searchField.stringValue.isEmpty {
            searchField.stringValue = ""; applyFilter(preferredID: selectedEntry?.id)
            content.view.window?.makeFirstResponder(tableView)
        } else { closeSwitcher() }
    }

    private func updateTools() {
        let menu = NSMenu()
        menu.addItem(withTitle: "…", action: nil, keyEquivalent: "")
        let utilities = utilitiesProvider().filter { !$0.visible }
        if !utilities.isEmpty {
            if menu.items.count > 1 { menu.addItem(.separator()) }
            for utility in utilities {
                addAction(NPText("Open %@", String(describing: (utility.title))), to: menu, enabled: utility.enabled) { [weak self] in
                    guard let self, self.bridgeProvider() === self.presentedBridge,
                          self.presentedBridge?.computerSessionID == self.presentedSession,
                          let current = self.utilitiesProvider().first(where: { $0.title == utility.title }), current.enabled else { return }
                    self.closeSwitcher(); current.action()
                }
            }
        }
        actionsButton.menu = menu
        actionsButton.isEnabled = menu.items.count > 1
        actionsButton.isHidden = menu.items.count <= 1
    }
    private func windowActions(for entry: Entry) -> [NSButton] {
        guard let window = entry.window else { return [] }
        func button(_ label: String, _ symbol: String, _ action: @escaping () -> Void) -> NSButton {
            let button = SwitcherActionButton()
            button.image = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)
            button.imagePosition = .imageOnly
            button.bezelStyle = .inline
            button.isBordered = false
            button.toolTip = label
            button.setAccessibilityLabel(NPText("%@: %@ on %@", String(describing: (label)), String(describing: (entry.title)), String(describing: (title))))
            button.perform = action; button.target = button
            button.action = #selector(SwitcherActionButton.invoke)
            button.translatesAutoresizingMaskIntoConstraints = false
            NSLayoutConstraint.activate([button.widthAnchor.constraint(equalToConstant: 24), button.heightAnchor.constraint(equalToConstant: 24)])
            return button
        }
        var buttons = [
            button(window.isMiniaturized ? NPText("Restore Window") : NPText("Minimize"), window.isMiniaturized ? "arrow.up.forward.app" : "minus") { [weak self] in
                self?.perform(entry) { window.isMiniaturized ? $0.activateDockWindow($1) : $0.minimizeDockWindow($1) }
            },
            button(window.isZoomed ? NPText("Restore Size") : NPText("Zoom"), window.isZoomed ? "minus.magnifyingglass" : "plus") { [weak self] in self?.perform(entry) { $0.toggleZoomDockWindow($1) } },
            button(window.isFullscreen ? NPText("Exit Full Screen") : NPText("Enter Full Screen"), window.isFullscreen ? "arrow.down.right.and.arrow.up.left" : "arrow.up.left.and.arrow.down.right") { [weak self] in self?.perform(entry) { $0.toggleFullscreenDockWindow($1) } },
            button(NPText("Close Window"), "xmark") { [weak self] in self?.perform(entry) { $0.requestCloseDockWindow($1) } },
        ]
        if window.canForceQuit {
            buttons.append(button(NPText("Force Quit Application…"), "stop.circle") { [weak self] in self?.forceQuit(entry) })
        }
        for (current, next) in zip(buttons, buttons.dropFirst()) { current.nextKeyView = next }
        buttons.last?.nextKeyView = searchField
        return buttons
    }
    private func focusSelectedActions() {
        guard selectedEntry?.window != nil,
              let cell = tableView.view(atColumn: 0, row: tableView.selectedRow, makeIfNecessary: true) as? SwitcherCell,
              let button = cell.firstActionButton else { return }
        button.window?.makeFirstResponder(button)
    }
    private func addAction(_ title: String, to menu: NSMenu, enabled: Bool = true, action: @escaping () -> Void) {
        let item = menu.addItem(withTitle: title, action: #selector(menuAction(_:)), keyEquivalent: "")
        item.target = self; item.isEnabled = enabled; item.representedObject = MenuAction(action)
        menu.autoenablesItems = false
    }
    @objc private func menuAction(_ sender: NSMenuItem) { (sender.representedObject as? MenuAction)?.perform() }

    @objc func showSelectedWindow() {
        guard let entry = selectedEntry else { return }
        if entry.window != nil { perform(entry) { $0.activateDockWindow($1) } }
        else if bridgeProvider() === presentedBridge,
                presentedBridge?.computerSessionID == presentedSession,
                let utility = utilitiesProvider().first(where: { "utility:\($0.title)" == entry.id }), utility.visible, utility.enabled {
            closeSwitcher(); utility.action()
        } else { refresh() }
    }
    private func currentBridge(for entry: Entry) -> WindowBridge? {
        guard let bridge = presentedBridge, bridgeProvider() === bridge,
              bridge.computerSessionID == presentedSession,
              let id = entry.window?.id, let native = bridge.window(id), native.window != nil,
              ObjectIdentifier(native) == entry.nativeID else { return nil }
        return bridge
    }
    private func perform(_ entry: Entry, action: (WindowBridge, UInt32) -> Bool) {
        guard let bridge = currentBridge(for: entry), let id = entry.window?.id else { refresh(); return }
        closeSwitcher()
        _ = action(bridge, id)
    }
    private func forceQuit(_ entry: Entry) {
        guard let bridge = currentBridge(for: entry), let window = entry.window else { refresh(); return }
        let session = bridge.computerSessionID
        closeSwitcher()
        let alert = NSAlert()
        alert.alertStyle = .critical
        alert.messageText = NPText("Force Quit %@?", String(describing: (window.applicationName ?? window.applicationID ?? window.title)))
        alert.informativeText = NPText("All windows of this application on %@ will close. Unsaved changes will be lost.", String(describing: (title)))
        alert.addButton(withTitle: NPText("Cancel"))
        alert.addButton(withTitle: NPText("Force Quit"))
        guard alert.runModal() == .alertSecondButtonReturn,
              bridgeProvider() === bridge, bridge.computerSessionID == session,
              bridge.window(window.id).map(ObjectIdentifier.init) == entry.nativeID else { return }
        _ = bridge.forceQuitDockWindow(window.id)
    }

    private func startObserving() {
        guard !observing else { return }; observing = true
        let center = NotificationCenter.default
        if let presentedBridge {
            center.addObserver(self, selector: #selector(windowsChanged), name: WindowBridge.dockWindowsDidChange, object: presentedBridge)
        }
        for name in nativeStateNotifications {
            center.addObserver(self, selector: #selector(windowsChanged), name: name, object: nil)
        }
        center.addObserver(self, selector: #selector(deactivated), name: NSApplication.didResignActiveNotification, object: NSApp)
    }
    @objc private func windowsChanged() {
        guard !refreshPending else { return }; refreshPending = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }; self.refreshPending = false; self.refresh()
        }
    }
    @objc private func deactivated() { closeSwitcher() }
    public func popoverWillShow(_ notification: Notification) {
        content.view.window?.initialFirstResponder = tableView
    }
    public func popoverDidShow(_ notification: Notification) {
        guard NSApp.isActive else { closeSwitcher(); return }
        content.view.window?.initialFirstResponder = tableView
        content.view.window?.makeKey()
        // Window selection is the primary task. Start on the list so an idle
        // chooser does not redraw a blinking text caret; typing begins search.
        content.view.window?.makeFirstResponder(tableView)
    }
    public func popoverDidClose(_ notification: Notification) {
        anchorPanel.orderOut(nil)
        // Remove only the subscriptions owned by this controller.
        let center = NotificationCenter.default
        center.removeObserver(self, name: WindowBridge.dockWindowsDidChange, object: presentedBridge)
        for name in nativeStateNotifications { center.removeObserver(self, name: name, object: nil) }
        center.removeObserver(self, name: NSApplication.didResignActiveNotification, object: NSApp)
        observing = false
        // Release snapshots and menu closures as soon as they are no longer visible.
        entries.removeAll(); filtered.removeAll(); actionsButton.menu = nil
        tableView.reloadData()
        presentedBridge = nil; presentedSession = nil
    }
    private func activateHost() {
        if NSApp.isHidden { NSApp.unhide(nil) }
        if !NSApp.isActive { NSApp.activate(ignoringOtherApps: true) }
    }
    private func closeSwitcher() {
        // Dismiss synchronously before restoring a guest's first responder.
        popover.animates = false
        popover.close()
        anchorPanel.orderOut(nil)
    }

    private func positionAnchor() -> NSRectEdge {
        let pointer = NSEvent.mouseLocation
        guard let screen = NSScreen.screens.first(where: { $0.frame.contains(pointer) }) ?? NSScreen.main else { return .maxY }
        let frame = screen.frame, visible = screen.visibleFrame
        content.preferredContentSize = NSSize(width: min(480, visible.width - 24),
                                              height: min(560, 154 + CGFloat(entries.count) * 78, visible.height - 24))
        let distances: [(NSRectEdge, CGFloat)] = [(.maxY, abs(pointer.y - frame.minY)),
            (.maxX, abs(pointer.x - frame.minX)), (.minX, abs(frame.maxX - pointer.x))]
        let edge = distances.min { $0.1 < $1.1 }?.0 ?? .maxY
        let x = min(max(pointer.x - 1, visible.minX), visible.maxX - 2)
        let y = min(max(pointer.y - 1, visible.minY), visible.maxY - 2)
        anchorPanel.setFrameOrigin(NSPoint(x: edge == .maxX ? visible.minX : (edge == .minX ? visible.maxX - 2 : x),
                                          y: edge == .maxY ? visible.minY : y))
        return edge
    }
}
