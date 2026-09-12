import Foundation
import NativePipeRemote

struct RemotePipeCLI {
    var command: SSHCommand?
    var wantHelp = false
    static let usage = """
    Usage: nativepipe [options] user@host application [arguments...]

    Launch a Linux application directly; its windows appear on this Mac.
    SSH handles authentication and communication without port forwarding.

      nativepipe user@host firefox --no-remote
      nativepipe -p 2222 --install-compositor user@host gtk4-demo

    Options (before user@host):
      --compositor PATH      Remote compositor executable
      --install-compositor   Install/update compositor from GitHub Release
      -i FILE, -F FILE, -J HOST, -p PORT, -o OPTION
                             Pass an authentication/connection option to SSH
      -h, --help             Show help
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
