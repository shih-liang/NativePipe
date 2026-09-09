import AppKit
import NativePipeRemote

@main
struct RemotePipeMain {
    @MainActor static func main() {
        if SSHAuthentication.answerPromptIfRequested() { return }
        do {
            let options = try RemotePipeCLI.parse(Array(CommandLine.arguments.dropFirst()))
            if options.wantHelp { print(RemotePipeCLI.usage); return }
            guard let command = options.command else { return }
            let app = NSApplication.shared
            app.setActivationPolicy(.regular)
            let delegate = RemoteApplicationDelegate(command: command)
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
