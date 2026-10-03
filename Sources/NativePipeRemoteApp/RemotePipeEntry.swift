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
            let showProgress = options.progress && isatty(STDERR_FILENO) != 0
            if showProgress { fputs("nativepipe: connecting over SSH; waiting for authentication and the Linux display helper…\n", stderr) }
            delegate.onConnected = { [weak delegate] in
                delegate?.hostIntegration.sync()
                if showProgress { fputs("nativepipe: connected; waiting for Linux application windows.\n", stderr) }
            }
            delegate.onFailure = { _ in
                fputs("Next: check ordinary SSH login with the same connection options. If the Linux helper is missing, retry with --install-compositor. See nativepipe --help and the session diagnostics below.\n", stderr)
            }
            delegate.onTerminate = { [weak delegate] in
                exit(delegate?.display.session.exitStatus ?? 0)
            }
            app.delegate = delegate
            withExtendedLifetime(delegate) { app.run() }
        } catch {
            fputs("nativepipe: \(error.localizedDescription)\n", stderr)
            exit(2)
        }
    }
}
