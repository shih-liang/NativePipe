import NativePipeStrings
import Foundation
import NativePipeRemote

struct RemotePipeCLI {
    var command: SSHCommand?
    var wantHelp = false
    var progress = true
    static let usage = NPText("cli.help.nativepiperemoteapp.cli")

    static func parse(_ arguments: [String]) throws -> Self {
        var result = Self(), index = 0
        var ssh: [String] = [], compositor = "nativepipe-wayland", install = false
        while index < arguments.count {
            let value = arguments[index]
            if value == "-h" || value == "--help" { result.wantHelp = true; return result }
            if value == "--no-progress" { result.progress = false; index += 1; continue }
            if value == "--install-compositor" { install = true; index += 1; continue }
            if value == "--compositor" || ["-i", "-F", "-J", "-p", "-o"].contains(value) {
                guard index + 1 < arguments.count else {
                    throw RemoteError.message(NPText("Missing value for %@. See nativepipe --help.", String(describing: (value))))
                }
                let parameter = arguments[index + 1]
                guard !parameter.isEmpty, !parameter.hasPrefix("-") else {
                    throw RemoteError.message(NPText("Missing or invalid option value. See nativepipe --help."))
                }
                if value == "-p" {
                    guard let port = Int(parameter), (1...65535).contains(port) else {
                        throw RemoteError.message(NPText("SSH port must be 1–65535. Use -p 2222 before the destination."))
                    }
                }
                if value == "--compositor" { compositor = parameter }
                else { ssh += [value, arguments[index + 1]] }
                index += 2
                continue
            }
            guard !value.hasPrefix("-") else {
                throw RemoteError.message(NPText("Unknown option. Options must precede the SSH destination; see nativepipe --help."))
            }
            result.command = SSHCommand(destination: value, application: Array(arguments.dropFirst(index + 1)),
                                        sshArguments: ssh, compositor: compositor,
                                        installCompositor: install)
            try result.command?.validate()
            return result
        }
        throw RemoteError.message(NPText("Specify an SSH destination and an application.\n%@", String(describing: (usage))))
    }
}
