import AppKit
import NativePipeRemote
import NativePipeStrings

@main
struct RemotePipeMain {
    @MainActor static func main() {
        let arguments = Array(CommandLine.arguments.dropFirst())
        // Even an inherited askpass environment must not make local help open UI.
        if let options = try? RemotePipeCLI.parse(arguments) {
            if options.wantHelp { print(RemotePipeCLI.usage); return }
            if options.wantVersion { print("nativepipe \(NativePipeVersion.current)"); return }
        }
        if SSHAuthentication.answerPromptIfRequested() { return }
        do {
            let options = try RemotePipeCLI.parse(arguments)
            if options.wantHelp { print(RemotePipeCLI.usage); return }
            if options.wantVersion { print("nativepipe \(NativePipeVersion.current)"); return }
            guard let command = options.command else { return }
            let app = NSApplication.shared
            app.setActivationPolicy(.regular)
            let delegate = RemoteApplicationDelegate(command: command)
            delegate.aboutPanelOptions = [
                .applicationName: "NativePipe",
                .applicationVersion: NativePipeVersion.current,
            ]
            let showProgress = options.progress && isatty(STDERR_FILENO) != 0
            let destination = command.destination
            let application = RemotePipeCLI.applicationName(command)
            func say(_ line: String) { fputs("nativepipe: " + line + "\n", stderr) }
            if showProgress { say(NPText("Connecting to %@…", destination)) }
            delegate.onConnected = { [weak delegate] in
                delegate?.hostIntegration.sync()
                if showProgress {
                    say(NPText("Connected. Waiting for %@ to open a window. Press Ctrl-C to disconnect.", application))
                }
            }
            // The outcome is always printed, progress or not: a redirected
            // stderr still needs to say why the session ended.
            delegate.reportsFailureToStderr = false
            delegate.onFailure = { [weak delegate] error in
                say(RemotePipeCLI.failureLine(
                    destination: destination, application: application,
                    connected: delegate?.hasConnected ?? false,
                    remoteExitStatus: delegate?.display.session.remoteExitStatus, error: error))
            }
            // Silence after dismissing the sign-in prompt reads as a hang or a bug.
            delegate.onDisconnected = { [weak delegate] in
                if delegate?.signInCancelled == true { say(NPText("Sign-in cancelled.")) }
            }
            delegate.onTerminate = { [weak delegate] in
                exit(RemotePipeCLI.exitCode(signInCancelled: delegate?.signInCancelled ?? false,
                                            sessionStatus: delegate?.display.session.exitStatus))
            }
            app.delegate = delegate
            withExtendedLifetime(delegate) { app.run() }
        } catch {
            fputs("nativepipe: \(error.localizedDescription)\n", stderr)
            exit(2)
        }
    }
}
