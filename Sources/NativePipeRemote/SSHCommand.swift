import Foundation

public struct SSHCommand: Codable, Sendable, Equatable {
    public var destination: String
    public var application: [String]
    public var sshArguments: [String]
    public var compositor: String
    public var installCompositor: Bool
    public var persistentSession: Bool = false

    public init(destination: String, application: [String], sshArguments: [String] = [],
                compositor: String = "nativepipe-wayland", installCompositor: Bool = false) {
        self.destination = destination
        self.application = application
        self.sshArguments = sshArguments
        self.compositor = compositor
        self.installCompositor = installCompositor
    }

    public func validate() throws {
        guard !destination.isEmpty, !destination.hasPrefix("-"),
              !destination.contains(where: { $0.isWhitespace || $0 == "\0" }),
              persistentSession || (!application.isEmpty && !application[0].isEmpty),
              !compositor.isEmpty,
              (application + sshArguments + [compositor]).allSatisfy({ !$0.contains("\0") })
        else { throw RemoteError.message("Enter an SSH destination and an application to run.") }
    }
    public var credentialID: String {
        let index = sshArguments.firstIndex(of: "-p")
        let port = index.flatMap { $0 + 1 < sshArguments.count ? sshArguments[$0 + 1] : nil } ?? "22"
        return destination + ":" + port
    }

    public static func quote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    public func arguments() async throws -> [String] {
        try await arguments(uploadCompositor: false)
    }

    func arguments(uploadCompositor: Bool,
                   hardwareH264: Bool = H264Decoder.hardwareAvailable) async throws -> [String] {
        try validate()
        var script = makeRemoteScript(uploadCompositor: uploadCompositor, hardwareH264: hardwareH264)
        if installCompositor && compositor == "nativepipe-wayland" && !uploadCompositor {
            let release = try await RemoteCompositorRelease.latest()
            script = "release=" + Self.quote(release.absoluteString) + "\n" + script
        }
        return ["-T", "-C", "-o", "ControlPath=none", "-o", "ClearAllForwardings=yes", "-o", "ExitOnForwardFailure=yes",
                "-o", "ConnectTimeout=15", "-o", "ServerAliveInterval=30", "-o", "ServerAliveCountMax=3"]
            + sshArguments + ["--", destination, "sh -c " + Self.quote(script)]
    }

    public var remoteScript: String {
        makeRemoteScript(uploadCompositor: false)
    }

    func makeRemoteScript(uploadCompositor: Bool,
                          hardwareH264: Bool = H264Decoder.hardwareAvailable) -> String {
        let invocation = persistentSession ? "--stdio --session" :
            "--stdio -- " + application.map(Self.quote).joined(separator: " ")
        let prepare = installCompositor ? (uploadCompositor ? RemoteCompositorInstaller.uploadScript : RemoteCompositorInstaller.script) : """
        if command -v nativepipe-wayland >/dev/null 2>&1; then
          compositor=$(command -v nativepipe-wayland)
        elif [ -x "$HOME/.local/share/nativepipe/compositor/current/nativepipe-wayland" ]; then
          compositor="$HOME/.local/share/nativepipe/compositor/current/nativepipe-wayland"
        elif [ -x "$HOME/.local/share/nativepipe/compositor/nativepipe-wayland" ]; then
          compositor="$HOME/.local/share/nativepipe/compositor/nativepipe-wayland"
        else
          echo 'NativePipe compositor is not installed. Reconnect with --install-compositor.' >&2
          exit 127
        fi
        """
        return """
        set -eu
        export NATIVEPIPE_HOST_H264_HARDWARE=\(hardwareH264 ? "1" : "0")
        compositor=\(Self.quote(compositor))
        if [ "$compositor" = nativepipe-wayland ]; then
          \(prepare)
        fi
        exec "$compositor" \(invocation)
        """
    }

}

public enum RemoteError: LocalizedError {
    case message(String)
    public var errorDescription: String? {
        switch self { case .message(let message): message }
    }
}
