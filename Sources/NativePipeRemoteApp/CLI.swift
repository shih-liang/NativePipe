import NativePipeStrings
import Foundation
import NativePipeRemote

struct RemotePipeCLI {
    var command: SSHCommand?
    var wantHelp = false
    var wantVersion = false
    var progress = true
    static let usage = NPText("cli.help.nativepiperemoteapp.cli")

    /// SSH switches that take no value and change neither who connects nor
    /// what the session carries: address family and agent forwarding.
    ///
    /// This is an allow-list on purpose. SSH's stdio is NativePipe's binary
    /// transport, so a pass-through for arbitrary flags would accept -t, which
    /// overrides the built-in -T and runs the frames through a terminal, or
    /// -N and -f, which leave no remote command at all. -l is absent too:
    /// user@host already covers it, and saved credentials are keyed by the
    /// destination, so -l would let two accounts on one host share an entry.
    static let sshSwitches: Set<String> = ["-4", "-6", "-A", "-a"]

    /// What to say about an SSH option NativePipe rejects on purpose. People
    /// reach for these from ordinary ssh use, and a bare "Unknown option" gives
    /// them nothing to go on. Echoing the option is safe only because it must
    /// match this fixed list exactly; anything else is never repeated back.
    static func rejectedSSHOption(_ option: String) -> String? {
        switch option {
        case "-l":
            return NPText("Use user@host instead of -l.")
        case "-t", "-tt":
            return NPText("%@ can’t be used with NativePipe: it would put a terminal in the display stream.", option)
        case "-N", "-f", "-W":
            return NPText("%@ can’t be used with NativePipe: it would stop SSH from running the compositor.", option)
        case "-L", "-R", "-D":
            return NPText("%@ isn’t supported: NativePipe sessions don’t forward ports.", option)
        case "-v", "-vv", "-vvv":
            return NPText("For SSH debugging output, use -o LogLevel=DEBUG instead of %@.", option)
        default:
            return nil
        }
    }

    static func parse(_ arguments: [String]) throws -> Self {
        var result = Self(), index = 0
        var ssh: [String] = [], compositor = "nativepipe-wayland", install = false
        while index < arguments.count {
            let value = arguments[index]
            if value == "-h" || value == "--help" { result.wantHelp = true; return result }
            if value == "--version" { result.wantVersion = true; return result }
            if value == "--no-progress" { result.progress = false; index += 1; continue }
            if value == "--install-compositor" { install = true; index += 1; continue }
            if sshSwitches.contains(value) { ssh.append(value); index += 1; continue }
            if value == "--compositor" || ["-i", "-F", "-J", "-p", "-o"].contains(value) {
                guard index + 1 < arguments.count else {
                    throw RemoteError.message(NPText("Missing value for %@. See nativepipe --help.", value))
                }
                let parameter = arguments[index + 1]
                // Names the option, never the value: an option name comes from
                // the fixed list above, while a value may be a mistyped secret.
                guard !parameter.isEmpty, !parameter.hasPrefix("-") else {
                    throw RemoteError.message(NPText("Missing value for %@. See nativepipe --help.", value))
                }
                if value == "-p" {
                    guard let port = Int(parameter), (1...65535).contains(port) else {
                        throw RemoteError.message(NPText("The SSH port must be a number from 1 to 65535."))
                    }
                }
                if value == "--compositor" { compositor = parameter }
                else { ssh += [value, arguments[index + 1]] }
                index += 2
                continue
            }
            guard !value.hasPrefix("-") else {
                throw RemoteError.message(rejectedSSHOption(value)
                    ?? NPText("Unknown option. Run nativepipe --help to see the supported options."))
            }
            result.command = SSHCommand(destination: value, application: Array(arguments.dropFirst(index + 1)),
                                        sshArguments: ssh, compositor: compositor,
                                        installCompositor: install)
            try result.command?.validate()
            return result
        }
        throw RemoteError.message(NPText("Specify an SSH destination and an application.\n%@", usage))
    }

    /// Exit status for a finished run. A cancelled sign-in exits 130, like
    /// Ctrl-C, which already ends the process with SIGINT: both mean the user
    /// stopped it. Exiting 0 there let `nativepipe … && next-step` carry on as
    /// though the application had run.
    static func exitCode(signInCancelled: Bool, sessionStatus: Int32?) -> Int32 {
        signInCancelled ? 130 : (sessionStatus ?? 0)
    }

    /// The program named in progress and failure lines: the first word of the
    /// remote command, without its directory.
    static func applicationName(_ command: SSHCommand) -> String {
        command.application.first.map { URL(fileURLWithPath: $0).lastPathComponent } ?? ""
    }

    /// The last line of a failed session: what happened, seen from the user's
    /// side, and what to check. By then SSH's and the compositor's own messages
    /// are already on screen above it, so this states the outcome and never
    /// repeats them.
    static func failureLine(destination: String, application: String, connected: Bool,
                            remoteExitStatus: Int32?, error: Error) -> String {
        guard let status = remoteExitStatus else {
            // This Mac rejected the session; its error is not on screen yet.
            let reason = error.localizedDescription.split(whereSeparator: \.isNewline)
                .first.map(String.init) ?? error.localizedDescription
            return connected ? NPText("Disconnected from %@: %@", destination, reason) : reason
        }
        switch (connected, status) {
        case (false, 255):
            // SSH itself failed: resolving, reaching or authenticating.
            return NPText("Couldn’t connect to %@. Make sure ssh %@ works with the same options, then try again.",
                          destination, destination)
        case (false, _):
            return NPText("Couldn’t start the session on %@. The messages above show why.", destination)
        case (true, 255):
            // After a session is up, 255 is SSH reporting a dropped connection,
            // not the application's own status.
            return NPText("Lost the connection to %@.", destination)
        case (true, 129...159):
            // The compositor reports a signal as 128 + its number.
            return NPText("%@ quit unexpectedly (signal %@).", application, String(status - 128))
        case (true, _):
            return NPText("%@ exited with status %@.", application, String(status))
        }
    }
}
