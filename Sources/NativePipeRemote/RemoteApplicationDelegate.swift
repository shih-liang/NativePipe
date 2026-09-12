import AppKit
import NativePipeWindowing

/// Shared app lifecycle for the standalone CLI and FluxWindow RemoteHost.
/// VMHost uses the same bridge, switcher and input command writer.
@MainActor
public final class RemoteApplicationDelegate: NSObject, NSApplicationDelegate {
    public let display: RemoteDisplayController
    public var onConnected: (() -> Void)?
    public var onDisconnected: (() -> Void)?
    public var onFailure: ((Error) -> Void)?
    public var onTerminate: (() -> Void)?
    public var exitOnDisconnect = true
    private let name: String
    private let showErrors: Bool
    private let loadsApplicationIcons: Bool
    private var stopping = false
    private var started = false
    private var connectionTask: Task<Void, Never>?
    public lazy var hostIntegration: HostIntegrationController = {
        let integration = HostIntegrationController()
        integration.applyWindows = { [weak self] in self?.display.bridge.setIntegrationPreferences($0) }
        integration.applyDesktop = { [weak self] value in
            guard let self, self.display.session.isConnected else { return }
            Task { @MainActor [weak self] in
                do { try await self?.display.session.applicationClient.setAppearance(value.colorScheme) }
                catch { fputs("[remote] appearance: \(error.localizedDescription)\n", stderr) }
            }
        }
        return integration
    }()
    private lazy var switcher = DockWindowSwitcherController(
        title: name, bridge: { [weak self] in self?.display.bridge })

    public init(command: SSHCommand, environment: [String: String]? = nil,
                showErrors: Bool = false, localCompositorDirectory: URL? = nil) {
        name = command.destination
        self.showErrors = showErrors
        loadsApplicationIcons = !command.persistentSession
        display = RemoteDisplayController(command: command, environment: environment,
                                          localCompositorDirectory: localCompositorDirectory)
        super.init()
    }

    public func applicationDidFinishLaunching(_ notification: Notification) {
        start()
    }
    public func start() {
        guard !started else { return }
        started = true
        NSApp.mainMenu = makeMenu()
        display.session.onDiagnostic = { text in fputs(text, stderr) }
        display.session.onError = { [weak self] error in self?.failed(error) }
        display.onStateChange = { [weak self] state in
            guard let self else { return }
            if state == .disconnected && !stopping {
                onDisconnected?()
                if exitOnDisconnect {
                    stopping = true
                    NSApp.terminate(nil)
                }
            }
        }
        connect()
    }
    public func connect() {
        connectionTask?.cancel()
        stopping = false
        connectionTask = Task {
            do {
                try await display.connect()
                try Task.checkCancellation()
                guard display.session.isConnected else { return }
                if let onConnected { onConnected() }
                else { hostIntegration.sync() }
                if loadsApplicationIcons { _ = try? await display.refreshApplications() }
            } catch is CancellationError { }
            catch { if !Task.isCancelled { failed(error) } }
        }
    }

    public func applicationShouldHandleReopen(_ sender: NSApplication,
                                              hasVisibleWindows flag: Bool) -> Bool {
        switcher.showSwitcher()
        return false
    }
    public func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false // The remote command's exit, not a transient empty window list, ends the session.
    }
    public func applicationWillTerminate(_ notification: Notification) {
        stopping = true
        connectionTask?.cancel()
        display.disconnect()
        onTerminate?()
    }
    private func failed(_ error: Error) {
        guard !stopping else { return }
        // Failure owns shutdown. Suppress the resulting disconnected callback
        // before releasing windows, pixels and SSH, then present the log once.
        stopping = true
        display.disconnect()
        onFailure?(error)
        fputs("nativepipe: \(error.localizedDescription)\n", stderr)
        if showErrors {
            let alert = Self.failureAlert(name: name, log: error.localizedDescription)
            NSApp.activate()
            alert.runModal()
        }
        if exitOnDisconnect { NSApp.terminate(nil) }
    }

    static func failureAlert(name: String, log: String) -> NSAlert {
        let alert = NSAlert()
        alert.messageText = "Couldn’t Connect to \(name)"
        alert.informativeText = "The connection has closed. You can try connecting again."
        alert.addButton(withTitle: "Close")
        let scroll = NSScrollView(frame: NSRect(x: 0, y: 0, width: 520, height: 240))
        scroll.hasVerticalScroller = true
        scroll.borderType = .bezelBorder
        let text = NSTextView(frame: scroll.bounds)
        text.isEditable = false
        text.isSelectable = true
        text.isVerticallyResizable = true
        text.autoresizingMask = [.width]
        text.textContainer?.widthTracksTextView = true
        text.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        text.string = log
        text.setAccessibilityLabel("Connection log")
        scroll.documentView = text
        alert.accessoryView = scroll
        return alert
    }
    private func makeMenu() -> NSMenu {
        let menu = NSMenu()
        let app = NSMenuItem()
        let actions = NSMenu(title: "NativePipe")
        actions.addItem(withTitle: "Disconnect from \(name)",
                        action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        app.submenu = actions
        menu.addItem(app)
        let item = NSMenuItem()
        let windows = NSMenu(title: "Window")
        windows.addItem(withTitle: "Minimize",
                        action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        windows.addItem(withTitle: "Bring All to Front",
                        action: #selector(NSApplication.arrangeInFront(_:)), keyEquivalent: "")
        item.submenu = windows
        menu.addItem(item)
        NSApp.windowsMenu = windows
        return menu
    }
}

/// OpenSSH invokes this executable only when authentication needs user input.
/// Credentials go to SSH through its askpass pipe, never a command argument,
/// preference file, or log. Host-key decisions remain OpenSSH's responsibility.
@MainActor
public enum SSHAuthentication {
    public static func answerPromptIfRequested() -> Bool {
        guard ProcessInfo.processInfo.environment["NATIVEPIPE_SSH_ASKPASS"] == "1" else {
            return false
        }
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        let environment = ProcessInfo.processInfo.environment
        let prompt = CommandLine.arguments.dropFirst().joined(separator: " ")
        let rememberable = SSHCredentialStore.mayRemember(prompt)
        let store = SSHCredentialStore(
            connection: environment["NATIVEPIPE_SSH_CONNECTION"] ?? "",
            accessGroup: environment["NATIVEPIPE_KEYCHAIN_GROUP"])
        if rememberable,
           SSHCredentialStore.claimCachedAttempt(
               prompt: prompt, directory: environment["NATIVEPIPE_SSH_AUTH_SESSION"]),
           let saved = try? store.read(prompt) {
            print(saved)
            return true
        }
        let alert = NSAlert()
        alert.messageText = "SSH Authentication"
        alert.informativeText = prompt
        let confirming = ProcessInfo.processInfo.environment["SSH_ASKPASS_PROMPT"] == "confirm"
        let input = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 360, height: 24))
        let remember = NSButton(checkboxWithTitle: "Remember in Keychain", target: nil, action: nil)
        remember.state = .on
        if !confirming {
            let content = NSStackView(views: rememberable ? [input, remember] : [input])
            content.orientation = .vertical
            content.alignment = .leading
            content.spacing = 10
            content.frame = NSRect(x: 0, y: 0, width: 360, height: rememberable ? 60 : 24)
            input.widthAnchor.constraint(equalToConstant: 360).isActive = true
            alert.accessoryView = content
        }
        alert.addButton(withTitle: confirming ? "Connect" : "Continue")
        alert.addButton(withTitle: "Cancel")
        if !confirming { alert.window.initialFirstResponder = input }
        app.activate()
        if alert.runModal() == .alertFirstButtonReturn {
            if rememberable {
                do {
                    if remember.state == .on { try store.save(input.stringValue, prompt: prompt) }
                    else { try store.remove(prompt) }
                } catch {
                    let warning = NSAlert()
                    warning.messageText = "Password Wasn’t Saved"
                    warning.informativeText = error.localizedDescription
                    warning.runModal()
                }
            }
            print(confirming ? "yes" : input.stringValue)
        } else { exit(EXIT_FAILURE) }
        return true
    }

    public static func environment(askpassExecutable: URL) -> [String: String] {
        var value = ProcessInfo.processInfo.environment
        value["SSH_ASKPASS"] = askpassExecutable.path
        value["SSH_ASKPASS_REQUIRE"] = "force"
        value["NATIVEPIPE_SSH_ASKPASS"] = "1"
        value["DISPLAY"] = ":0"
        return value
    }
}
