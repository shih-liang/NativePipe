import Foundation
import NativePipeProtocol

/// Manual argv split so ssh flags (`-i`, `-p`, `-o`, …) pass through untouched.
struct RemotePipeCLI {
    var destination: String?
    var compositor: String = "nativepipe-wayland"
    var host: String = "127.0.0.1"
    var surfacePort: UInt16 = UInt16(NativePipePort.surface)
    var mediaPort: UInt16 = UInt16(NativePipePort.media)
    var sshArguments: [String] = []
    var wantHelp = false

    static let usage = """
        Usage:
          nativepipe user@host [ssh-args…] [--compositor PATH]
          nativepipe --host 127.0.0.1 [--surface-port N] [--media-port N]

        One-shot SSH session (recommended):
          Opens local forwards for ports 1025/1026, starts the remote compositor
          if needed, runs the NativePipe display client, then drops you into a
          remote login shell with WAYLAND_DISPLAY set. Exit the shell to tear down.

          nativepipe lfs@172.16.0.34
          nativepipe user@host --compositor ~/bin/nativepipe-wayland
          nativepipe user@host -i ~/.ssh/id_ed25519 -p 2222
          nativepipe user@host -- -o ProxyJump=bastion

        Local-only (manual ssh -L already set up):
          nativepipe --host 127.0.0.1

        Options:
          --compositor PATH   Remote compositor binary (default: nativepipe-wayland)
          --host ADDR         Local mode: connect to ADDR (default 127.0.0.1)
          --surface-port N    Local mode / override local forward port (default 1025)
          --media-port N      Local mode / override local forward port (default 1026)
          -h, --help          Show this help
        """

    static func parse(_ arguments: [String] = Array(CommandLine.arguments.dropFirst())) throws -> RemotePipeCLI {
        var cli = RemotePipeCLI()
        var i = arguments.startIndex
        while i < arguments.endIndex {
            let arg = arguments[i]
            if arg == "--" {
                cli.sshArguments.append(contentsOf: arguments[arguments.index(after: i)...])
                break
            }
            if arg == "-h" || arg == "--help" {
                cli.wantHelp = true
                i = arguments.index(after: i)
                continue
            }
            if arg == "--compositor" {
                i = arguments.index(after: i)
                guard i < arguments.endIndex else {
                    throw CLIError.missingValue("--compositor")
                }
                cli.compositor = arguments[i]
                i = arguments.index(after: i)
                continue
            }
            if arg.hasPrefix("--compositor=") {
                cli.compositor = String(arg.dropFirst("--compositor=".count))
                i = arguments.index(after: i)
                continue
            }
            if arg == "--host" {
                i = arguments.index(after: i)
                guard i < arguments.endIndex else {
                    throw CLIError.missingValue("--host")
                }
                cli.host = arguments[i]
                i = arguments.index(after: i)
                continue
            }
            if arg.hasPrefix("--host=") {
                cli.host = String(arg.dropFirst("--host=".count))
                i = arguments.index(after: i)
                continue
            }
            if arg == "--surface-port" {
                i = arguments.index(after: i)
                guard i < arguments.endIndex, let port = UInt16(arguments[i]) else {
                    throw CLIError.missingValue("--surface-port")
                }
                cli.surfacePort = port
                i = arguments.index(after: i)
                continue
            }
            if arg.hasPrefix("--surface-port=") {
                guard let port = UInt16(arg.dropFirst("--surface-port=".count)) else {
                    throw CLIError.missingValue("--surface-port")
                }
                cli.surfacePort = port
                i = arguments.index(after: i)
                continue
            }
            if arg == "--media-port" {
                i = arguments.index(after: i)
                guard i < arguments.endIndex, let port = UInt16(arguments[i]) else {
                    throw CLIError.missingValue("--media-port")
                }
                cli.mediaPort = port
                i = arguments.index(after: i)
                continue
            }
            if arg.hasPrefix("--media-port=") {
                guard let port = UInt16(arg.dropFirst("--media-port=".count)) else {
                    throw CLIError.missingValue("--media-port")
                }
                cli.mediaPort = port
                i = arguments.index(after: i)
                continue
            }
            if !arg.hasPrefix("-"), cli.destination == nil {
                cli.destination = arg
                i = arguments.index(after: i)
                continue
            }
            cli.sshArguments.append(arg)
            if Self.sshOptionTakesValue(arg),
               arguments.index(after: i) < arguments.endIndex
            {
                let next = arguments[arguments.index(after: i)]
                if cli.destination == nil, !next.hasPrefix("-"), looksLikeDestination(next) {
                    // leave next for destination parsing
                } else {
                    cli.sshArguments.append(next)
                    i = arguments.index(after: i)
                }
            }
            i = arguments.index(after: i)
        }
        return cli
    }

    private static func looksLikeDestination(_ value: String) -> Bool {
        value.contains("@") || (!value.contains("/") && value.contains("."))
    }

    private static func sshOptionTakesValue(_ arg: String) -> Bool {
        let singles: Set<Character> = [
            "b", "c", "D", "E", "e", "F", "I", "i", "J", "L", "l",
            "m", "O", "o", "p", "Q", "R", "S", "W", "w",
        ]
        if arg.hasPrefix("--") { return false }
        guard arg.hasPrefix("-"), arg.count >= 2 else { return false }
        if arg.count > 2 { return false }
        return singles.contains(arg[arg.index(after: arg.startIndex)])
    }
}

enum CLIError: Error, CustomStringConvertible {
    case missingValue(String)
    case invalidUsage(String)

    var description: String {
        switch self {
        case .missingValue(let option):
            return "missing value for \(option)"
        case .invalidUsage(let message):
            return message
        }
    }
}
