import AppKit
import SwiftUI

@MainActor
public final class DockWindowSwitcherController: NSObject, NSPopoverDelegate {
    private enum DockEdge {
        case bottom, left, right

        var popoverEdge: NSRectEdge {
            switch self {
            case .bottom: .maxY
            case .left: .maxX
            case .right: .minX
            }
        }
    }

    public struct Utility {
        public let title: String
        public let symbol: String
        public let visible: Bool
        public let enabled: Bool
        public let action: () -> Void

        public init(title: String, symbol: String, visible: Bool, enabled: Bool,
                    action: @escaping () -> Void) {
            self.title = title
            self.symbol = symbol
            self.visible = visible
            self.enabled = enabled
            self.action = action
        }
    }

    private let title: String
    private let bridgeProvider: () -> WindowBridge?
    private let utilitiesProvider: () -> [Utility]
    private let anchorView = NSView(frame: NSRect(x: 0, y: 0, width: 2, height: 2))
    private let anchorPanel: NSPanel
    private let popover = NSPopover()
    private lazy var host = NSHostingController(rootView: makeView())

    public init(title: String, bridge: @escaping () -> WindowBridge?,
                utilities: @escaping () -> [Utility] = { [] }) {
        self.title = title
        self.bridgeProvider = bridge
        self.utilitiesProvider = utilities
        anchorPanel = NSPanel(
            contentRect: anchorView.frame,
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false)
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
        popover.animates = true
        popover.delegate = self
        popover.contentViewController = host

        // The nonactivating anchor does not reliably dismiss its transient
        // popover when a different application becomes active.
        NotificationCenter.default.addObserver(
            self, selector: #selector(applicationDidResignActive(_:)),
            name: NSApplication.didResignActiveNotification, object: NSApp)
    }

    public func showSwitcher() {
        if popover.isShown {
            popover.performClose(nil)
            return
        }
        host.rootView = makeView()
        let edge = positionAnchor()
        anchorPanel.orderFrontRegardless()
        popover.show(relativeTo: anchorView.bounds, of: anchorView, preferredEdge: edge)
    }

    public func popoverDidShow(_ notification: Notification) {
        guard popover.isShown, NSApp.isActive else { return }
        // The invisible anchor is deliberately nonactivating. Showing its
        // popover alone leaves the previous guest window key, so give focus
        // to the switcher once AppKit has created and shown its window.
        host.view.window?.makeKey()
    }

    public func popoverDidClose(_ notification: Notification) {
        anchorPanel.orderOut(nil)
    }

    @objc private func applicationDidResignActive(_ notification: Notification) {
        closeSwitcher()
    }

    private func makeView() -> VMWindowSwitcherView {
        let bridge = bridgeProvider()
        let windows = bridge?.dockWindows.map {
            SwitcherWindow(window: $0, icon: bridge?.iconForDockWindow($0.id))
        } ?? []
        return VMWindowSwitcherView(
            machineName: title,
            machineIcon: NSApp.applicationIconImage,
            windows: windows,
            utilities: utilitiesProvider().map { item in
                Utility(title: item.title, symbol: item.symbol, visible: item.visible,
                        enabled: item.enabled) { [weak self] in
                    self?.closeSwitcher()
                    item.action()
                }
            },
            onSelectWindow: { [weak self] id in self?.selectWindow(id) },
            onMinimizeWindow: { [weak self] id in self?.minimizeWindow(id) },
            onToggleZoomWindow: { [weak self] id in self?.toggleZoomWindow(id) },
            onToggleFullscreenWindow: { [weak self] id in self?.toggleFullscreenWindow(id) },
            onCloseWindow: { [weak self] id in self?.closeWindow(id) },
            onForceQuitWindow: { [weak self] id, title in
                self?.forceQuitWindow(id, title: title)
            })
    }

    private func positionAnchor() -> NSRectEdge {
        let pointer = NSEvent.mouseLocation
        guard let screen = NSScreen.screens.first(where: { $0.frame.contains(pointer) })
                ?? NSScreen.main else {
            return .maxY
        }
        let frame = screen.frame
        let visible = screen.visibleFrame
        let edge: DockEdge
        if pointer.y < visible.minY {
            edge = .bottom
        } else if pointer.x < visible.minX {
            edge = .left
        } else if pointer.x > visible.maxX {
            edge = .right
        } else {
            let distances: [(DockEdge, CGFloat)] = [
                (.bottom, abs(pointer.y - frame.minY)),
                (.left, abs(pointer.x - frame.minX)),
                (.right, abs(frame.maxX - pointer.x)),
            ]
            edge = distances.min { $0.1 < $1.1 }?.0 ?? .bottom
        }

        let origin: NSPoint
        switch edge {
        case .bottom:
            origin = NSPoint(
                x: min(max(pointer.x - 1, visible.minX), visible.maxX - 2),
                y: visible.minY)
        case .left:
            origin = NSPoint(
                x: visible.minX,
                y: min(max(pointer.y - 1, visible.minY), visible.maxY - 2))
        case .right:
            origin = NSPoint(
                x: visible.maxX - 2,
                y: min(max(pointer.y - 1, visible.minY), visible.maxY - 2))
        }
        anchorPanel.setFrameOrigin(origin)
        return edge.popoverEdge
    }

    private func closeSwitcher() {
        popover.performClose(nil)
    }

    private func selectWindow(_ id: UInt32) {
        closeSwitcher()
        _ = bridgeProvider()?.activateDockWindow(id)
    }

    private func minimizeWindow(_ id: UInt32) {
        closeSwitcher()
        _ = bridgeProvider()?.minimizeDockWindow(id)
    }

    private func toggleZoomWindow(_ id: UInt32) {
        closeSwitcher()
        _ = bridgeProvider()?.toggleZoomDockWindow(id)
    }

    private func toggleFullscreenWindow(_ id: UInt32) {
        closeSwitcher()
        _ = bridgeProvider()?.toggleFullscreenDockWindow(id)
    }

    private func closeWindow(_ id: UInt32) {
        closeSwitcher()
        _ = bridgeProvider()?.requestCloseDockWindow(id)
    }

    private func forceQuitWindow(_ id: UInt32, title: String) {
        closeSwitcher()
        let alert = NSAlert()
        alert.alertStyle = .critical
        alert.messageText = "Force Quit \(title)?"
        alert.informativeText =
            "Unsaved changes will be lost. All windows owned by this guest " +
            "application process will close."
        alert.addButton(withTitle: "Force Quit")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        _ = bridgeProvider()?.forceQuitDockWindow(id)
    }


}

private struct SwitcherWindow: Identifiable {
    let window: DockWindow
    let icon: NSImage?

    var id: UInt32 { window.id }
}

private struct VMWindowSwitcherView: View {
    let machineName: String
    let machineIcon: NSImage?
    let windows: [SwitcherWindow]
    let utilities: [DockWindowSwitcherController.Utility]
    let onSelectWindow: (UInt32) -> Void
    let onMinimizeWindow: (UInt32) -> Void
    let onToggleZoomWindow: (UInt32) -> Void
    let onToggleFullscreenWindow: (UInt32) -> Void
    let onCloseWindow: (UInt32) -> Void
    let onForceQuitWindow: (UInt32, String) -> Void

    private var height: CGFloat {
        let windowRows = max(windows.count, 1)
        return min(590, 140 + CGFloat((windowRows + utilities.count) * 44))
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                if let machineIcon {
                    Image(nsImage: machineIcon)
                        .resizable()
                        .scaledToFit()
                        .frame(width: 28, height: 28)
                }
                Text(machineName)
                    .font(.headline)
                    .lineLimit(1)
                Spacer()
            }
            .padding(.horizontal, 12)
            .frame(height: 50)

            Divider()

            ScrollView {
                LazyVStack(spacing: 2) {
                    SectionHeader(title: "Open Windows", systemImage: "macwindow")

                    if windows.isEmpty {
                        HStack {
                            Image(systemName: "macwindow")
                            Text("No application windows")
                                .foregroundStyle(.secondary)
                            Spacer()
                        }
                        .padding(.horizontal, 10)
                        .frame(height: 42)
                    } else {
                        ForEach(windows) { window in
                            SwitcherRow(
                                title: window.window.title,
                                subtitle: window.window.applicationID,
                                systemImage: "app.fill",
                                icon: window.icon,
                                windowActions: WindowContextActions(
                                    isMiniaturized: window.window.isMiniaturized,
                                    isZoomed: window.window.isZoomed,
                                    isFullscreen: window.window.isFullscreen,
                                    show: { onSelectWindow(window.id) },
                                    minimize: { onMinimizeWindow(window.id) },
                                    toggleZoom: { onToggleZoomWindow(window.id) },
                                    toggleFullscreen: {
                                        onToggleFullscreenWindow(window.id)
                                    },
                                    close: { onCloseWindow(window.id) },
                                    forceQuit: window.window.canForceQuit ? {
                                        onForceQuitWindow(window.id, window.window.title)
                                    } : nil),
                                action: { onSelectWindow(window.id) })
                        }
                    }

                    if !utilities.isEmpty {
                        Divider().padding(.vertical, 6)
                        SectionHeader(title: "System", systemImage: "gearshape")
                        ForEach(utilities.indices, id: \.self) { index in
                            let item = utilities[index]
                            SwitcherRow(title: item.title, systemImage: item.symbol,
                                        visible: item.visible, enabled: item.enabled,
                                        action: item.action)
                        }
                    }
                }
                .padding(8)
            }
        }
        .frame(width: 390, height: height)
    }
}

private struct SectionHeader: View {
    let title: String
    let systemImage: String

    var body: some View {
        HStack(spacing: 6) {
            Label(title, systemImage: systemImage)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            Spacer()
        }
        .padding(.horizontal, 8)
        .frame(height: 30)
    }
}

private struct WindowContextActions {
    let isMiniaturized: Bool
    let isZoomed: Bool
    let isFullscreen: Bool
    let show: () -> Void
    let minimize: () -> Void
    let toggleZoom: () -> Void
    let toggleFullscreen: () -> Void
    let close: () -> Void
    let forceQuit: (() -> Void)?
}

private struct SwitcherRow: View {
    let title: String
    var subtitle: String?
    let systemImage: String
    var icon: NSImage? = nil
    var visible = false
    var enabled = true
    var windowActions: WindowContextActions? = nil
    let action: () -> Void

    @State private var hovering = false

    private var row: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                Group {
                    if let icon {
                        Image(nsImage: icon)
                            .resizable()
                            .scaledToFit()
                    } else {
                        Image(systemName: systemImage)
                            .font(.system(size: 16))
                    }
                }
                .frame(width: 24, height: 24)
                VStack(alignment: .leading, spacing: 1) {
                    Text(title).lineLimit(1)
                    if let subtitle, !subtitle.isEmpty {
                        Text(subtitle)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                }
                Spacer()
                if visible {
                    Image(systemName: "checkmark")
                        .foregroundStyle(.secondary)
                        .accessibilityLabel("Visible")
                }
            }
            .padding(.horizontal, 10)
            .frame(minHeight: 42)
            .contentShape(Rectangle())
            .background(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(hovering ? Color.primary.opacity(0.08) : .clear))
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.45)
        .onHover { hovering = $0 }
    }

    @ViewBuilder
    var body: some View {
        if let windowActions {
            row.contextMenu {
                Button("Show", action: windowActions.show)
                Divider()
                Button("Minimize", action: windowActions.minimize)
                    .disabled(windowActions.isMiniaturized)
                Button(
                    windowActions.isZoomed ? "Restore" : "Zoom",
                    action: windowActions.toggleZoom)
                Button(
                    windowActions.isFullscreen ? "Exit Full Screen" : "Enter Full Screen",
                    action: windowActions.toggleFullscreen)
                Divider()
                Button("Close", action: windowActions.close)
                if let forceQuit = windowActions.forceQuit {
                    Divider()
                    Button("Force Quit…", role: .destructive, action: forceQuit)
                }
            }
        } else {
            row
        }
    }
}
