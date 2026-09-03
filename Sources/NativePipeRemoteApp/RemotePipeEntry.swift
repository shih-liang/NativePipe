import AppKit
import Foundation
import NativePipeProtocol
import NativePipeRemote
#if canImport(Darwin)
import Darwin
#endif

/// `nativepipe` — SSH orchestration only. Display/protocol live in NativePipe*.
@main
enum RemotePipeMain {
    static func main() {
        setvbuf(stdout, nil, _IOLBF, 0)
        setvbuf(stderr, nil, _IONBF, 0)

        let cli: RemotePipeCLI
        do {
            cli = try RemotePipeCLI.parse()
        } catch {
            fputs("nativepipe: \(error)\n\n\(RemotePipeCLI.usage)\n", stderr)
            exit(2)
        }
        if cli.wantHelp {
            print(RemotePipeCLI.usage)
            exit(0)
        }

        MainActor.assumeIsolated {
            let app = NSApplication.shared
            app.setActivationPolicy(.regular)
            let delegate = RemotePipeAppDelegate(cli: cli)
            app.delegate = delegate
            withExtendedLifetime(delegate) { app.run() }
        }
    }
}

@MainActor
final class RemotePipeAppDelegate: NSObject, NSApplicationDelegate {
    private let cli: RemotePipeCLI
    private var session: DisplaySession?
    private var tunnelProcess: Process?
    private var shellProcess: Process?

    init(cli: RemotePipeCLI) {
        self.cli = cli
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        installMenu()

        if let destination = cli.destination {
            startSSHSession(destination: destination)
        } else {
            startLocalSession()
        }
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    func applicationWillTerminate(_ notification: Notification) {
        if let process = shellProcess, process.isRunning {
            process.terminate()
        }
        if let process = tunnelProcess, process.isRunning {
            process.terminate()
        }
        session?.disconnect()
    }

    // MARK: - modes

    private func startLocalSession() {
        let session = DisplaySession(
            host: cli.host,
            surfacePort: cli.surfacePort,
            mediaPort: cli.mediaPort)
        self.session = session
        session.onStateChange = { [weak self] state in
            guard let self else { return }
            switch state {
            case .connected:
                fputs(
                    "nativepipe: display connected to \(self.cli.host):"
                        + "\(self.cli.surfacePort)/\(self.cli.mediaPort)\n",
                    stderr)
                fputs("nativepipe: waiting for remote windows…\n", stderr)
            case .disconnected:
                fputs("nativepipe: display disconnected\n", stderr)
            }
        }
        Task { @MainActor [weak self, weak session] in
            guard let self, let session, self.session === session else { return }
            do {
                try await session.connect()
                fputs(
                    "nativepipe: transport established; waiting for compositor handshake…\n",
                    stderr)
            } catch is CancellationError {
                return
            } catch {
                fputs("nativepipe: connect failed: \(error)\n", stderr)
                fputs(
                    """
                    Tip: use one-shot SSH mode, or forward ports manually:

                      nativepipe user@host
                      # or:
                      ssh -N -L 1025:127.0.0.1:1025 -L 1026:127.0.0.1:1026 user@linux
                      nativepipe --host 127.0.0.1

                    """,
                    stderr)
                NSApp.terminate(nil)
            }
        }
    }

    private func startSSHSession(destination: String) {
        let forwards: SSHBootstrap.ForwardPorts
        let tunnel: Process
        do {
            forwards = try SSHBootstrap.allocateLocalPorts(
                preferredSurface: cli.surfacePort,
                preferredMedia: cli.mediaPort)
            fputs(
                "nativepipe: forwarding localhost:\(forwards.surface)/\(forwards.media) "
                    + "→ \(destination):\(NativePipePort.surface)/\(NativePipePort.media)\n",
                stderr)
            try SSHBootstrap.ensureCompositor(
                destination: destination,
                compositor: cli.compositor,
                sshArguments: cli.sshArguments)
            tunnel = try SSHBootstrap.startTunnel(
                destination: destination,
                sshArguments: cli.sshArguments,
                forwards: forwards)
            tunnelProcess = tunnel
            fputs("nativepipe: ssh tunnel ready\n", stderr)
        } catch {
            fputs("nativepipe: \(error)\n", stderr)
            NSApp.terminate(nil)
            return
        }

        // Display path is entirely NativePipeRemote — CLI only connects it.
        let session = DisplaySession(
            host: "127.0.0.1",
            surfacePort: forwards.surface,
            mediaPort: forwards.media)
        self.session = session
        session.onStateChange = { state in
            switch state {
            case .connected:
                fputs(
                    "nativepipe: display connected on 127.0.0.1:"
                        + "\(forwards.surface)/\(forwards.media)\n",
                    stderr)
                fputs("nativepipe: waiting for remote windows…\n", stderr)
            case .disconnected:
                fputs("nativepipe: display disconnected\n", stderr)
            }
        }
        Task { @MainActor [weak self, weak session] in
            guard let self, let session, self.session === session else { return }
            await self.finishSSHSession(
                session: session, tunnel: tunnel,
                destination: destination)
        }
    }

    private func finishSSHSession(
        session: DisplaySession, tunnel: Process,
        destination: String
    ) async {
        do {
            try await session.connect()
            fputs(
                "nativepipe: transport established; waiting for compositor handshake…\n",
                stderr)
        } catch is CancellationError {
            return
        } catch {
            fputs("nativepipe: display connect failed: \(error)\n", stderr)
            tunnel.terminate()
            NSApp.terminate(nil)
            return
        }

        if isatty(FileHandle.standardInput.fileDescriptor) == 0 {
            fputs(
                "nativepipe: stdin is not a TTY — display-only mode "
                    + "(run from Terminal for a remote shell; Quit to exit)\n",
                stderr)
            return
        }

        let shell: Process
        do {
            fputs("nativepipe: starting interactive ssh shell…\n", stderr)
            fflush(stderr)
            shell = try SSHBootstrap.startInteractiveShell(
                destination: destination,
                sshArguments: cli.sshArguments)
            shellProcess = shell
            fputs("nativepipe: ssh shell pid=\(shell.processIdentifier)\n", stderr)
            fflush(stderr)
        } catch {
            fputs("nativepipe: failed to start ssh shell: \(error)\n", stderr)
            tunnel.terminate()
            session.disconnect()
            NSApp.terminate(nil)
            return
        }

        let handleShellExit: @MainActor @Sendable (Int32) -> Void = { [weak self] status in
            if status != 0 {
                fputs("nativepipe: ssh shell exited \(status)\n", stderr)
            }
            self?.session?.disconnect()
            if let tunnel = self?.tunnelProcess, tunnel.isRunning {
                tunnel.terminate()
            }
            NSApp.terminate(nil)
        }
        DispatchQueue.global(qos: .userInitiated).async {
            shell.waitUntilExit()
            let status = shell.terminationStatus
            Task { @MainActor in
                handleShellExit(status)
            }
        }
    }

    private func installMenu() {
        let menu = NSMenu()
        let appMenuItem = NSMenuItem()
        menu.addItem(appMenuItem)
        let appMenu = NSMenu()
        appMenu.addItem(
            withTitle: "Quit NativePipe",
            action: #selector(NSApplication.terminate(_:)),
            keyEquivalent: "q")
        appMenuItem.submenu = appMenu
        NSApp.mainMenu = menu
    }
}
