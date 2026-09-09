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

    public func arguments() throws -> [String] {
        try validate()
        return ["-T", "-o", "ClearAllForwardings=yes", "-o", "ExitOnForwardFailure=yes",
                "-o", "ServerAliveInterval=30", "-o", "ServerAliveCountMax=3"]
            + sshArguments + ["--", destination, "sh -c " + Self.quote(remoteScript)]
    }

    public var remoteScript: String {
        let invocation = persistentSession ? "--stdio --session" :
            "--stdio -- " + application.map(Self.quote).joined(separator: " ")
        let install = installCompositor ? Self.installScript : """
        echo 'NativePipe compositor is not installed. Install it on the remote computer, or reconnect with --install-compositor.' >&2
        exit 127
        """
        return """
        set -eu
        compositor=\(Self.quote(compositor))
        if [ "$compositor" = nativepipe-wayland ]; then
          if command -v nativepipe-wayland >/dev/null 2>&1; then
            compositor=$(command -v nativepipe-wayland)
          elif [ -x "$HOME/.local/share/nativepipe/compositor/nativepipe-wayland" ]; then
            compositor="$HOME/.local/share/nativepipe/compositor/nativepipe-wayland"
          else
            \(install)
          fi
        fi
        exec "$compositor" \(invocation)
        """
    }

    // No sudo or distribution modification. Image libraries are private;
    // FFmpeg/VA-API, GLib/GIO and graphics drivers come from the system.
    private static let installScript = """
    arch=$(uname -m)
    case "$arch" in aarch64|x86_64) ;; *) echo "Unsupported architecture: $arch" >&2; exit 1;; esac
    libc=gnu
    if ldd --version 2>&1 | head -1 | grep -qi musl; then libc=musl; fi
    command -v curl >/dev/null || { echo 'Install curl to download NativePipe.' >&2; exit 1; }
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT HUP INT TERM
    asset="nativepipe-compositor-$arch-$libc.tar.gz"
    base=https://github.com/shih-liang/nativepipe/releases/latest/download
    echo "Downloading NativePipe compositor for $arch ($libc)…" >&2
    curl --fail --location --proto '=https' --tlsv1.2 "$base/$asset" -o "$tmp/$asset" >&2
    curl --fail --location --proto '=https' --tlsv1.2 "$base/SHA256SUMS" -o "$tmp/SHA256SUMS" >&2
    (cd "$tmp" && grep "  $asset$" SHA256SUMS | sha256sum -c - >&2)
    mkdir "$tmp/unpacked"
    tar -xzf "$tmp/$asset" -C "$tmp/unpacked"
    mkdir -p "$HOME/.local/share/nativepipe/compositor"
    cp -R "$tmp/unpacked/." "$HOME/.local/share/nativepipe/compositor/"
    compositor="$HOME/.local/share/nativepipe/compositor/nativepipe-wayland"
    [ -x "$compositor" ] || { echo 'Release is missing nativepipe-wayland.' >&2; exit 1; }
    """
}

public enum RemoteError: LocalizedError {
    case message(String)
    public var errorDescription: String? {
        switch self { case .message(let message): message }
    }
}
