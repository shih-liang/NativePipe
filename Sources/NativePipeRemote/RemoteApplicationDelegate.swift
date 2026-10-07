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
    /// Whether a failure is also written to stderr as one raw line. A host that
    /// presents failures itself -- the CLI writes a summary keyed to what went
    /// wrong -- turns this off so the terminal does not get both.
    public var reportsFailureToStderr = true
    /// True once this attempt reached the Linux application. Decides whether a
    /// failure is "couldn't connect" or "disconnected": the user has very
    /// different things to check in each case.
    public private(set) var hasConnected = false
    /// True when this attempt ended because the user dismissed the sign-in
    /// prompt. Not a failure, and not a success either: the session never ran.
    public private(set) var signInCancelled = false
    public var displayName: String {
        didSet {
            guard displayName != oldValue else { return }
            switcher.title = displayName
            display.bridge.machineName = displayName
            disconnectItem?.title = NPText("Disconnect from %@", displayName)
        }
    }
    private weak var disconnectItem: NSMenuItem?
    /// Adds an About item to the app menu when set. Off by default: this
    /// delegate is shared with embedding hosts, which own their own About and
    /// must not end up showing NativePipe's. A bare executable has no
    /// Info.plist for the standard panel to read, so the caller supplies the
    /// name and version explicitly.
    public var aboutPanelOptions: [NSApplication.AboutPanelOptionKey: Any]?
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
                catch { fputs("nativepipe: " + NPText("Couldn’t match the Linux applications to the macOS appearance: %@", error.localizedDescription) + "\n", stderr) }
            }
        }
        return integration
    }()
    private lazy var switcher = DockWindowSwitcherController(
        title: displayName, bridge: { [weak self] in self?.display.bridge })

    public init(command: SSHCommand, environment: [String: String]? = nil,
                showErrors: Bool = false, localCompositorDirectory: URL? = nil) {
        displayName = command.destination
        self.showErrors = showErrors
        loadsApplicationIcons = !command.persistentSession
        display = RemoteDisplayController(command: command, environment: environment,
                                          localCompositorDirectory: localCompositorDirectory)
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
        hasConnected = false
        signInCancelled = false
        connectionTask = Task {
            do {
                try await display.connect()
                try Task.checkCancellation()
                guard display.session.isConnected else { return }
                hasConnected = true
                if let onConnected { onConnected() }
                else { hostIntegration.sync() }
                if loadsApplicationIcons { _ = try? await display.refreshApplications() }
            } catch is CancellationError {
                // A user dismissing askpass cancels the attempt as well. A
                // superseded Task must not stop the replacement connection.
                guard !Task.isCancelled else { return }
                stopping = true
                signInCancelled = true
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
        if reportsFailureToStderr { fputs("nativepipe: \(error.localizedDescription)\n", stderr) }
        if showErrors {
            let session = display.session
            let alert = Self.failureAlert(
                name: displayName, connected: hasConnected,
                summary: Self.failureSummary(error: error, remoteExitStatus: session.remoteExitStatus,
                                             diagnostics: session.visibleDiagnostics),
                log: session.visibleDiagnostics)
            NSApp.activate()
            alert.runModal()
        }
        if exitOnDisconnect { NSApp.terminate(nil) }
    }

    /// One sentence saying what went wrong, for the alert's informative text.
    ///
    /// A failure this Mac detected already is that sentence. When the remote
    /// command exited, its error is SSH's or the compositor's stderr instead,
    /// and the cause is usually the last thing written before the exit -- an
    /// installer's progress or a toolkit's warnings come earlier.
    nonisolated static func failureSummary(error: Error, remoteExitStatus: Int32?, diagnostics: String) -> String {
        let lines = { (text: String) in
            text.split(whereSeparator: \.isNewline)
                .map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        }
        if remoteExitStatus != nil, let last = lines(diagnostics).last { return last }
        return lines(error.localizedDescription).first ?? error.localizedDescription
    }

    /// The title says which side of a successful connection the failure is on,
    /// because the user checks different things in each case: SSH access and
    /// the compositor before, the application and the network after. The log
    /// is shown only when it says more than the summary already does.
    static func failureAlert(name: String, connected: Bool, summary: String, log: String) -> NSAlert {
        let alert = NSAlert()
        alert.messageText = connected
            ? NPText("Disconnected from %@", name)
            : NPText("Couldn’t Connect to %@", name)
        alert.informativeText = summary
        alert.addButton(withTitle: NPText("Close"))
        let trimmedLog = log.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedLog.isEmpty, trimmedLog != summary else { return alert }
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
        text.string = trimmedLog
        text.setAccessibilityLabel(NPText("Connection Log"))
        scroll.documentView = text
        alert.accessoryView = scroll
        return alert
    }
    private func makeMenu() -> NSMenu {
        let menu = NSMenu()
        let app = NSMenuItem()
        let actions = NSMenu(title: "NativePipe")
        if aboutPanelOptions != nil {
            let about = actions.addItem(withTitle: NPText("About NativePipe"),
                                        action: #selector(showAboutPanel(_:)), keyEquivalent: "")
            about.target = self
            actions.addItem(.separator())
        }
        disconnectItem = actions.addItem(withTitle: NPText("Disconnect from %@", displayName),
                        action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        app.submenu = actions
        menu.addItem(app)
        menu.addItem(StandardEditMenu.item())
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

    @objc private func showAboutPanel(_ sender: Any?) {
        NSApp.activate(ignoringOtherApps: true)
        NSApp.orderFrontStandardAboutPanel(options: aboutPanelOptions ?? [:])
    }
}

/// Cut, Copy, Paste and Select All reach AppKit text controls only through the
/// main menu's key equivalents. Without this menu the SSH password prompt, the
/// window switcher's search field and the connection log all lose them.
///
/// Guest windows are unaffected. Their content view implements none of these
/// actions, so over guest content every item validates as disabled and its
/// key equivalent falls through to keyDown, where the shortcut translation
/// handles it exactly as it did before (Cmd-C becomes the Linux app's Ctrl-C).
/// Translated chords never even get this far: the content view claims them in
/// performKeyEquivalent, which AppKit consults before the main menu.
@MainActor
enum StandardEditMenu {
    static func item() -> NSMenuItem {
        let menu = NSMenu(title: NPText("Edit"))
        menu.addItem(withTitle: NPText("Undo"), action: Selector(("undo:")), keyEquivalent: "z")
        let redo = menu.addItem(withTitle: NPText("Redo"), action: Selector(("redo:")), keyEquivalent: "z")
        redo.keyEquivalentModifierMask = [.command, .shift]
        menu.addItem(.separator())
        menu.addItem(withTitle: NPText("Cut"), action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        menu.addItem(withTitle: NPText("Copy"), action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        menu.addItem(withTitle: NPText("Paste"), action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        menu.addItem(withTitle: NPText("Select All"), action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        let item = NSMenuItem()
        item.submenu = menu
        return item
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
        let confirming = environment["SSH_ASKPASS_PROMPT"] == "confirm"
        let rememberable = !confirming && SSHCredentialStore.mayRemember(prompt)
        let loginPassword = SSHCredentialStore.isLoginPasswordPrompt(prompt)
        let store = credentialStore(connection: environment["NATIVEPIPE_SSH_CONNECTION"] ?? "", environment: environment)
        if let saved = try? store.cachedResponse(prompt: prompt, confirming: confirming,
                                                directory: environment["NATIVEPIPE_SSH_AUTH_SESSION"]) {
            print(saved)
            return true
        }
        // Askpass enters a modal loop without NSApplication.run(). Complete
        // AppKit startup so the prompt is registered and keyboard-accessible.
        app.finishLaunching()
        // Without it, Cmd-V cannot paste a password from a password manager.
        // The menu bar stays hidden for an accessory process, but AppKit still
        // matches key equivalents against it -- the first item is the
        // application menu's slot, so it holds an empty placeholder.
        let bar = NSMenu()
        let placeholder = NSMenuItem()
        placeholder.submenu = NSMenu()
        bar.addItem(placeholder)
        bar.addItem(StandardEditMenu.item())
        app.mainMenu = bar
        let alert = NSAlert()
        alert.messageText = NPText("Sign In to %@", environment["NATIVEPIPE_SSH_CONNECTION"] ?? NPText("Remote Computer"))
        alert.informativeText = prompt
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
                    if loginPassword {
                        if remember.state == .on { try store.saveLoginPassword(input.stringValue) }
                        else { try store.removeLoginPassword() }
                        // A replaced/forgotten typed password must not revive
                        // the legacy value for this actual prompt next time.
                        try store.remove(prompt)
                    } else if remember.state == .on { try store.save(input.stringValue, prompt: prompt) }
                    else { try store.remove(prompt) }
                } catch {
                    // Unchecking Remember asks for removal, and that can fail
                    // too; titling it "Wasn't Saved" would describe the opposite
                    // of what the user just asked for. Either way the sign-in
                    // itself continues -- the reply is printed below.
                    let saving = remember.state == .on
                    let warning = NSAlert()
                    warning.messageText = saving
                        ? NPText("Password Wasn’t Saved")
                        : NPText("Saved Password Wasn’t Removed")
                    warning.informativeText = saving
                        ? NPText("You’re still signing in, but NativePipe will ask for this password again next time.\n\n%@", error.localizedDescription)
                        : NPText("You’re still signing in, but the saved password remains in your Keychain.\n\n%@", error.localizedDescription)
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
    /// Prefer OpenSSH's explicit password method when the user saved a login
    /// password. Interactive challenges and public-key methods remain available.
    public static func preferredAuthenticationArguments(connection: String, environment: [String: String], sshArguments: [String] = []) -> [String] {
        guard !hasExplicitAuthenticationPreference(sshArguments) else { return [] }
        let saved = !connection.isEmpty && (try? credentialStore(connection: connection, environment: environment).hasLoginPassword()) == true
        return authenticationArguments(hasLoginPassword: saved, sshArguments: sshArguments)
    }
    static func authenticationArguments(hasLoginPassword: Bool, sshArguments: [String] = []) -> [String] {
        hasLoginPassword && !hasExplicitAuthenticationPreference(sshArguments)
            ? ["-o", "PreferredAuthentications=password,gssapi-with-mic,hostbased,publickey,keyboard-interactive"] : []
    }
    private static func hasExplicitAuthenticationPreference(_ arguments: [String]) -> Bool {
        for (index, argument) in arguments.enumerated() {
            let option: String
            if argument == "-o", index + 1 < arguments.count { option = arguments[index + 1] }
            else if argument.hasPrefix("-o") { option = String(argument.dropFirst(2)) }
            else { continue }
            if option.split(whereSeparator: { $0 == "=" || $0.isWhitespace }).first?.lowercased() == "preferredauthentications" {
                return true
            }
        }
        return false
    }
    private static func credentialStore(connection: String, environment: [String: String]) -> SSHCredentialStore {
        SSHCredentialStore(connection: connection, accessGroup: environment["NATIVEPIPE_KEYCHAIN_GROUP"],
            trustedApplications: environment["NATIVEPIPE_KEYCHAIN_APPLICATIONS"]?
                .split(separator: "\n").map { URL(fileURLWithPath: String($0)) } ?? [])
    }
}
