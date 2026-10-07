import AppKit
import NativePipeProtocol
import NativePipeStrings

/// Native sheets and the system Save panel own layout and sandbox grants.
/// They remain asynchronous so another application can become active normally.
@MainActor
public enum GuestOpenDialogs {
    public static func confirm(_ prompt: GuestOpenPrompt) async -> GuestOpenAction {
        guard !Task.isCancelled else { return .cancel }
        let alert = NSAlert()
        let machine: String
        switch prompt {
        case .link(let name, let url):
            machine = name
            alert.messageText = NPText("Open a link from %@?", name)
            alert.informativeText = GuestOpenPolicy.displayURL(url) + "\n\n" + NPText("The application for this link will open on your Mac. Mail links may include attachments; review the draft before sending.")
            if url.user != nil || url.password != nil {
                alert.informativeText += "\n" + NPText("This link includes sign-in credentials, which are hidden here.")
            }
            alert.addButton(withTitle: NPText("Open"))
            alert.addButton(withTitle: NPText("Cancel"))
        case .receive(let name, let path):
            machine = name
            alert.messageText = NPText("Receive an item from %@?", name)
            alert.informativeText = path + "\n\n" + NPText("Save a copy without opening it, or receive and open it on your Mac. Programs, scripts and installers need a separate confirmation before opening.")
            alert.addButton(withTitle: NPText("Save…"))
            alert.addButton(withTitle: NPText("Open"))
            alert.addButton(withTitle: NPText("Cancel"))
        case .execute(let name, let file):
            machine = name
            alert.alertStyle = .warning
            alert.messageText = NPText("Open this program or installer?")
            alert.informativeText = NPText("%@ was received from %@. Opening it may run code or change your Mac. Allow this item to open?", file.lastPathComponent, name)
            alert.addButton(withTitle: NPText("Open"))
            alert.addButton(withTitle: NPText("Cancel"))
        }
        // Return is deliberately not a shortcut for running guest content.
        alert.buttons.first?.keyEquivalent = ""
        alert.buttons.last?.keyEquivalent = "\r"
        let response = await present(alert, machine: machine)
        guard !Task.isCancelled else { return .cancel }
        if case .receive(_, let path) = prompt {
            if response == .alertFirstButtonReturn {
                let name = GuestOpenPolicy.safeFileName(URL(fileURLWithPath: path).lastPathComponent)
                let panel = NSOpenPanel()
                panel.canChooseDirectories = true
                panel.canChooseFiles = false
                panel.allowsMultipleSelection = false
                panel.canCreateDirectories = true
                panel.title = NPText("Save Received Item")
                panel.message = NPText("Choose where to save %@.", name)
                panel.prompt = NPText("Save")
                let saved = await withTaskCancellationHandler(operation: {
                    guard !Task.isCancelled else { return NSApplication.ModalResponse.cancel }
                    return await panel.begin()
                }, onCancel: {
                    Task { @MainActor in panel.cancel(nil) }
                })
                return !Task.isCancelled && saved == .OK ? panel.url.map { .save($0.appendingPathComponent(name)) } ?? .cancel : .cancel
            }
            return response == .alertSecondButtonReturn ? .open : .cancel
        }
        return response == .alertFirstButtonReturn ? .open : .cancel
    }

    private static func present(_ alert: NSAlert, machine: String) async -> NSApplication.ModalResponse {
        let existing = NSApp.keyWindow ?? NSApp.orderedWindows.first(where: { $0.isVisible && $0.canBecomeKey && $0.sheetParent == nil })
        let owner = existing ?? NSWindow(contentRect: NSRect(x: 0, y: 0, width: 480, height: 160),
            styleMask: [.titled, .closable], backing: .buffered, defer: false)
        if existing == nil {
            owner.isReleasedWhenClosed = false
            owner.title = machine
            owner.center()
            owner.makeKeyAndOrderFront(nil)
        }
        NSApp.activate(ignoringOtherApps: true)
        defer { if existing == nil { owner.close() } }
        return await withTaskCancellationHandler(operation: {
            guard !Task.isCancelled else { return .cancel }
            return await alert.beginSheetModal(for: owner)
        }, onCancel: {
            Task { @MainActor in owner.endSheet(alert.window, returnCode: .cancel) }
        })
    }
}

@MainActor
final class GuestOpenTransferWindow: NSObject, NSWindowDelegate {
    private let window: NSWindow
    private let indicator = NSProgressIndicator()
    private let detail = NSTextField(labelWithString: "")
    private let cancel: () -> Void

    init(name: String, machine: String, cancel: @escaping () -> Void) {
        self.cancel = cancel
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 460, height: 140),
            styleMask: [.titled, .closable], backing: .buffered, defer: false)
        super.init()
        window.isReleasedWhenClosed = false
        window.title = NPText("Receiving from %@", machine)
        window.delegate = self
        let heading = NSTextField(labelWithString: name)
        heading.lineBreakMode = .byTruncatingMiddle
        indicator.style = .bar
        indicator.isIndeterminate = true
        indicator.startAnimation(nil)
        detail.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        detail.textColor = .secondaryLabelColor
        let button = NSButton(title: NPText("Cancel"), target: self, action: #selector(cancelTransfer))
        let stack = NSStackView(views: [heading, indicator, detail, button])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false
        window.contentView?.addSubview(stack)
        if let content = window.contentView {
            NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
                stack.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -20),
                stack.topAnchor.constraint(equalTo: content.topAnchor, constant: 16),
                indicator.widthAnchor.constraint(equalTo: stack.widthAnchor)])
        }
        window.center()
        window.orderFront(nil)
    }

    func update(_ progress: FileTransferProgress) {
        if let total = progress.totalBytes, total > 0 {
            indicator.isIndeterminate = false
            indicator.maxValue = Double(total)
            indicator.doubleValue = Double(progress.bytesTransferred)
        }
        let count = ByteCountFormatter.string(fromByteCount: Int64(clamping: progress.bytesTransferred), countStyle: .file)
        detail.stringValue = progress.relativePath.isEmpty ? count : progress.relativePath + " · " + count
    }
    func close() { window.delegate = nil; window.close() }
    func windowShouldClose(_ sender: NSWindow) -> Bool { cancel(); return false }
    @objc private func cancelTransfer() { cancel() }
}
