import NativePipeStrings
import AppKit
import NativePipeWindowing

/// Shared app lifecycle for the standalone CLI and LinPortal RemoteHost.
/// VMHost uses the same bridge, switcher and input command writer.
@MainActor
public final class RemoteApplicationDelegate: NSObject, NSApplicationDelegate {
    public let display: RemoteDisplayController
    public var onConnected: (() -> Void)?
    public var onDisconnected: (() -> Void)?
    public var onFailure: ((Error) -> Void)?
    public var onTerminate: (() -> Void)?
    public var onDiagnostic: ((String) -> Void)?
    public var exitOnDisconnect = true
    public var displayName: String {
        didSet {
            guard displayName != oldValue else { return }
            switcher.title = displayName
            display.bridge.machineName = displayName
            disconnectItem?.title = NPText("Disconnect from %@", String(describing: (displayName)))
        }
    }
    private weak var disconnectItem: NSMenuItem?
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
        title: displayName, bridge: { [weak self] in self?.display.bridge })

    public init(command: SSHCommand, environment: [String: String]? = nil,
                showErrors: Bool = false, localCompositorDirectory: URL? = nil, clipboardFileDirectory: URL? = nil) {
        displayName = command.destination
        self.showErrors = showErrors
        loadsApplicationIcons = !command.persistentSession
        display = RemoteDisplayController(command: command, environment: environment,
                                          localCompositorDirectory: localCompositorDirectory,
                                          clipboardFileDirectory: clipboardFileDirectory)
        super.init()
        display.bridge.machineName = displayName
    }

    public func applicationDidFinishLaunching(_ notification: Notification) {
        start()
    }
    public func start() {
        guard !started else { return }
        started = true
        NSApp.mainMenu = makeMenu()
        display.session.onDiagnostic = { [weak self] text in
            fputs(text, stderr)
            self?.onDiagnostic?(text)
        }
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
            } catch is CancellationError {
                // A user dismissing askpass cancels the attempt as well. A
                // superseded Task must not stop the replacement connection.
                guard !Task.isCancelled else { return }
                stopping = true
                onDisconnected?()
                if exitOnDisconnect { NSApp.terminate(nil) }
            }
            catch { if !Task.isCancelled { failed(error) } }
        }
    }

    public func applicationShouldHandleReopen(_ sender: NSApplication,
                                              hasVisibleWindows flag: Bool) -> Bool {
        switcher.showWindows()
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
            let alert = Self.failureAlert(name: displayName, log: error.localizedDescription)
            NSApp.activate()
            alert.runModal()
        }
        if exitOnDisconnect { NSApp.terminate(nil) }
    }

    static func failureAlert(name: String, log: String) -> NSAlert {
        let alert = NSAlert()
        alert.messageText = NPText("Couldn’t Connect to %@", String(describing: (name)))
        alert.informativeText = NPText("The connection has closed. You can try connecting again.")
        alert.addButton(withTitle: NPText("Close"))
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
        text.setAccessibilityLabel(NPText("Connection Log"))
        scroll.documentView = text
        alert.accessoryView = scroll
        return alert
    }
    private func makeMenu() -> NSMenu {
        let menu = NSMenu()
        let app = NSMenuItem()
        let actions = NSMenu(title: "NativePipe")
        disconnectItem = actions.addItem(withTitle: NPText("Disconnect from %@", String(describing: (displayName))),
                        action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        app.submenu = actions
        menu.addItem(app)
        let item = NSMenuItem()
        let windows = NSMenu(title: NPText("Window"))
        windows.addItem(withTitle: NPText("Minimize"),
                        action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        windows.addItem(withTitle: NPText("Bring All to Front"),
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
    static let cancelledDiagnostic = "NATIVEPIPE AUTH CANCELLED\n"
    public static func answerPromptIfRequested() -> Bool {
        guard ProcessInfo.processInfo.environment["NATIVEPIPE_SSH_ASKPASS"] == "1" else {
            return false
        }
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        let environment = ProcessInfo.processInfo.environment
        if let path = environment["NATIVEPIPE_SSH_AUTH_SESSION"],
           !FileManager.default.fileExists(atPath: path) {
            fputs(cancelledDiagnostic, stderr)
            exit(EXIT_FAILURE)
        }
        // stderr belongs to the SSH diagnostic pipe, never the credential reply.
        fputs("NATIVEPIPE PHASE AUTHENTICATING\n", stderr)
        let prompt = CommandLine.arguments.dropFirst().joined(separator: " ")
        let rememberable = SSHCredentialStore.mayRemember(prompt)
        let store = SSHCredentialStore(
            connection: environment["NATIVEPIPE_SSH_CONNECTION"] ?? "",
            accessGroup: environment["NATIVEPIPE_KEYCHAIN_GROUP"],
            trustedApplications: environment["NATIVEPIPE_KEYCHAIN_APPLICATIONS"]?
                .split(separator: "\n").map { URL(fileURLWithPath: String($0)) } ?? [])
        if rememberable,
           SSHCredentialStore.claimCachedAttempt(
               prompt: prompt, directory: environment["NATIVEPIPE_SSH_AUTH_SESSION"]),
           let saved = try? store.read(prompt) {
            print(saved)
            return true
        }
        // Askpass enters a modal loop without NSApplication.run(). Complete
        // AppKit startup so the prompt is registered and keyboard-accessible.
        app.finishLaunching()
        let alert = NSAlert()
        alert.messageText = NPText("Sign In to %@", String(describing: (environment["NATIVEPIPE_SSH_CONNECTION"] ?? "Remote Computer")))
        alert.informativeText = prompt
        let confirming = ProcessInfo.processInfo.environment["SSH_ASKPASS_PROMPT"] == "confirm"
        if confirming { alert.messageText = NPText("Trust This Remote Computer?") }
        let input = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 360, height: 24))
        input.placeholderString = rememberable ? NPText("Password or passphrase") : NPText("Authentication response")
        input.setAccessibilityLabel(input.placeholderString)
        let remember = NSButton(checkboxWithTitle: NPText("Remember in Keychain"), target: nil, action: nil)
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
        alert.addButton(withTitle: confirming ? NPText("Connect") : NPText("Continue"))
        alert.addButton(withTitle: NPText("Cancel"))
        if !confirming { alert.window.initialFirstResponder = input }
        // Disconnect removes the attempt directory. Close a pending native
        // prompt as well, even if OpenSSH's askpass child outlives SSH itself.
        let watch: DispatchSourceFileSystemObject?
        if let path = environment["NATIVEPIPE_SSH_AUTH_SESSION"] {
            let fd = open(path, O_EVTONLY | O_CLOEXEC)
            if fd >= 0 {
                let source = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: .delete, queue: .main)
                source.setEventHandler { app.abortModal(); alert.window.orderOut(nil) }
                source.setCancelHandler { close(fd) }
                source.resume()
                watch = source
            } else { fputs(cancelledDiagnostic, stderr); exit(EXIT_FAILURE) }
        } else { watch = nil }
        defer { watch?.cancel() }
        app.activate()
        if alert.runModal() == .alertFirstButtonReturn {
            if rememberable {
                do {
                    if remember.state == .on { try store.save(input.stringValue, prompt: prompt) }
                    else { try store.remove(prompt) }
                } catch {
                    let warning = NSAlert()
                    warning.messageText = NPText("Password Wasn’t Saved")
                    warning.informativeText = error.localizedDescription
                    warning.runModal()
                }
            }
            print(confirming ? "yes" : input.stringValue)
        } else {
            // End this attempt for every SSH consumer, including SFTP. Later
            // authentication methods must not reopen a cancelled prompt.
            if let path = environment["NATIVEPIPE_SSH_AUTH_SESSION"] {
                try? FileManager.default.removeItem(atPath: path)
            }
            fputs(cancelledDiagnostic, stderr)
            exit(EXIT_FAILURE)
        }
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
