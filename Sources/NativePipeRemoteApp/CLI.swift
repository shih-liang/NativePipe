import Foundation
import NativePipeRemote

struct RemotePipeCLI {
    var command: SSHCommand?
    var wantHelp = false
    static let usage = """
    Usage: nativepipe [options] user@host application [arguments...]

    Waypipe for macOS: run Linux apps over SSH in native Mac windows.
    SSH handles authentication and communication without port forwarding.

      nativepipe --install-compositor user@host gtk4-demo
      nativepipe user@host firefox --no-remote
      nativepipe -p 2222 -i ~/.ssh/id_ed25519 user@host qterminal

    Options (before user@host):
      --compositor PATH      Remote executable (default: nativepipe-wayland)
                             A custom path bypasses automatic installation
      --install-compositor   Install/update compositor from GitHub Release
      -i FILE, -F FILE, -J HOST, -p PORT, -o OPTION
                             Pass an authentication/connection option to SSH
      -h, --help             Show help

    The Linux application must already be installed. --install-compositor installs
    only the display helper, as your SSH user. It does not require root access.
    After the destination, all arguments (including --help) belong to the Linux app.
    Keep the app in the foreground: its exit ends the session.

    Guide: https://github.com/shih-liang/NativePipe#usage
    """

    static func parse(_ arguments: [String]) throws -> Self {
        var result = Self(), index = 0
        var ssh: [String] = [], compositor = "nativepipe-wayland", install = false
        while index < arguments.count {
            let value = arguments[index]
            if value == "-h" || value == "--help" { result.wantHelp = true; return result }
            if value == "--install-compositor" { install = true; index += 1; continue }
            if value == "--compositor" || ["-i", "-F", "-J", "-p", "-o"].contains(value) {
                guard index + 1 < arguments.count else {
                    throw RemoteError.message("Missing value for \(value).")
                }
                if value == "--compositor" { compositor = arguments[index + 1] }
                else { ssh += [value, arguments[index + 1]] }
                index += 2
                continue
            }
            guard !value.hasPrefix("-") else {
                throw RemoteError.message("Unknown option: \(value). Options must precede the SSH destination.")
            }
            result.command = SSHCommand(destination: value, application: Array(arguments.dropFirst(index + 1)),
                                        sshArguments: ssh, compositor: compositor,
                                        installCompositor: install)
            try result.command?.validate()
            return result
        }
        throw RemoteError.message("Specify an SSH destination and an application.\n\(usage)")
    }
}
