import Foundation
import NativePipeProtocol
#if canImport(Darwin)
import Darwin
#endif

/// Local free-port picking + remote compositor ensure + SSH tunnel + shell.
enum SSHBootstrap {
    struct ForwardPorts {
        var surface: UInt16
        var media: UInt16
    }

    /// Prefer the protocol defaults; if busy, bind ephemeral ports.
    static func allocateLocalPorts(
        preferredSurface: UInt16 = UInt16(NativePipePort.surface),
        preferredMedia: UInt16 = UInt16(NativePipePort.media)
    ) throws -> ForwardPorts {
        let surface = try isPortFree(preferredSurface) ? preferredSurface : reserveEphemeralPort()
        var media = preferredMedia
        if media == surface || !isPortFree(media) {
            media = try reserveEphemeralPort()
            if media == surface {
                media = try reserveEphemeralPort()
            }
        }
        return ForwardPorts(surface: surface, media: media)
    }

    static func ensureCompositor(
        destination: String,
        compositor: String,
        sshArguments: [String]
    ) throws {
        let script = ensureScript(compositor: compositor)
        // OpenSSH joins argv after the destination with spaces — pass one
        // remote command string so `bash -lc '<script>'` stays intact.
        let remote = "bash -lc \(shellEscape(script))"
        let result = try runSSH(
            destination: destination,
            sshArguments: sshArguments,
            remoteCommand: remote,
            allocateTTY: false,
            inheritStdio: false)
        if result.status != 0 {
            let err = String(data: result.stderr, encoding: .utf8) ?? ""
            let out = String(data: result.stdout, encoding: .utf8) ?? ""
            throw CLIError.invalidUsage(
                """
                failed to ensure remote compositor (exit \(result.status))
                \(out)\(err)
                """)
        }
        let out = String(data: result.stdout, encoding: .utf8) ?? ""
        if !out.contains("ready") {
            throw CLIError.invalidUsage("compositor did not become ready:\n\(out)")
        }
        fputs("nativepipe: \(out.trimmingCharacters(in: .whitespacesAndNewlines))\n", stderr)
    }

    /// Background `ssh -N` holding LocalForwards. Survives without a TTY.
    static func startTunnel(
        destination: String,
        sshArguments: [String],
        forwards: ForwardPorts
    ) throws -> Process {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/ssh")
        process.arguments =
            [
                "-N",
                "-o", "BatchMode=yes",
                "-o", "ExitOnForwardFailure=yes",
                "-o", "ServerAliveInterval=30",
                "-L", "\(forwards.surface):127.0.0.1:\(NativePipePort.surface)",
                "-L", "\(forwards.media):127.0.0.1:\(NativePipePort.media)",
            ]
            + sshArguments
            + [destination]
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.standardError
        try process.run()

        // Wait until both local listeners are bound (or the tunnel dies).
        // Do not TCP-connect through the forward here — that would open a
        // short-lived session against the compositor's accept loop.
        let deadline = Date().addingTimeInterval(8)
        while Date() < deadline {
            if !process.isRunning {
                throw CLIError.invalidUsage("ssh tunnel exited before forwards were ready")
            }
            if !isPortFree(forwards.surface) && !isPortFree(forwards.media) {
                Thread.sleep(forTimeInterval: 0.05)
                return process
            }
            Thread.sleep(forTimeInterval: 0.1)
        }
        process.terminate()
        throw CLIError.invalidUsage(
            "timed out waiting for local forwards on \(forwards.surface)/\(forwards.media)")
    }

    /// Interactive login shell with WAYLAND_DISPLAY; caller waits on the process.
    /// Forwards are held by a separate `-N` tunnel — this session is shell-only.
    static func startInteractiveShell(
        destination: String,
        sshArguments: [String]
    ) throws -> Process {
        let remote = """
            RUNTIME="/tmp/nativepipe-xdg-$(id -u)"; \
            ENV="$RUNTIME/nativepipe-wayland.env"; \
            if [ ! -f "$ENV" ]; then \
              echo "nativepipe: missing $ENV (is the compositor running?)" >&2; \
              exit 1; \
            fi; \
            . "$ENV"; \
            if [ -z "${WAYLAND_DISPLAY:-}" ] || [ -z "${XDG_RUNTIME_DIR:-}" ]; then \
              echo "nativepipe: $ENV incomplete" >&2; \
              exit 1; \
            fi; \
            export WAYLAND_DISPLAY XDG_RUNTIME_DIR; \
            [ -z "${DISPLAY:-}" ] || export DISPLAY; \
            [ -z "${XAUTHORITY:-}" ] || export XAUTHORITY; \
            [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] || export DBUS_SESSION_BUS_ADDRESS; \
            printf 'WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s\\n' "$WAYLAND_DISPLAY" "$XDG_RUNTIME_DIR" \
              > /tmp/nativepipe-shell-started; \
            printf 'nativepipe: remote shell WAYLAND_DISPLAY=%s (runtime %s)\\n' \
              "$WAYLAND_DISPLAY" "$XDG_RUNTIME_DIR" >&2; \
            exec "${SHELL:-/bin/bash}" -l
            """
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/ssh")
        process.arguments =
            [
                "-tt",
                "-o", "RequestTTY=force",
            ]
            + sshArguments
            + [destination, "bash -lc \(shellEscape(remote))"]
        // Inherit stdin/stdout/stderr from this process (the Terminal TTY).
        // Explicit FileHandle.standard* assignment is unreliable once AppKit runs.
        try process.run()
        // AppKit is the session leader; an interactive ssh that inherits the
        // Terminal TTY gets SIGTTIN and stops (ps state T) unless it is moved
        // into the foreground process group.
        let pid = process.processIdentifier
        if pid > 0, isatty(STDIN_FILENO) != 0 {
            signal(SIGTTOU, SIG_IGN)
            signal(SIGTTIN, SIG_IGN)
            _ = setpgid(pid, pid)
            _ = tcsetpgrp(STDIN_FILENO, pid)
            kill(pid, SIGCONT)
        }
        return process
    }

    // MARK: - helpers

    static func ensureScript(compositor: String) -> String {
        let path = shellEscape(compositor)
        // Dedicated XDG_RUNTIME_DIR so a session compositor that already owns
        // $XDG_RUNTIME_DIR/wayland-0 cannot collide with ours. Clients in the
        // The NativePipe ssh shell inherits the same runtime + display variables.
        return """
            set -e
            COMPOSITOR=\(path)
            RUNTIME="/tmp/nativepipe-xdg-$(id -u)"
            LOG="$RUNTIME/nativepipe-wayland.log"
            ENV="$RUNTIME/nativepipe-wayland.env"
            PID="$RUNTIME/nativepipe-wayland.pid"
            mkdir -p "$RUNTIME"
            chmod 700 "$RUNTIME"
            if ! command -v "$COMPOSITOR" >/dev/null 2>&1 && [ ! -x "$COMPOSITOR" ]; then
              echo "nativepipe: compositor not found: $COMPOSITOR" >&2
              exit 127
            fi
            listening() {
              if command -v ss >/dev/null 2>&1; then
                sockets=$(ss -ltn 2>/dev/null)
              else
                sockets=$(netstat -ltn 2>/dev/null)
              fi
              printf '%s\\n' "$sockets" | grep -Eq '[:.]1025[[:space:]]' &&
                printf '%s\\n' "$sockets" | grep -Eq '[:.]1026[[:space:]]'
            }
            owned_compositor() {
              tracked_alive || return 1
              listening
            }
            tracked_alive() {
              [ -s "$PID" ] || return 1
              pid=$(cat "$PID" 2>/dev/null) || return 1
              case "$pid" in *[!0-9]*|'') return 1 ;; esac
              kill -0 "$pid" 2>/dev/null
            }
            if ! owned_compositor; then
              if tracked_alive; then
                echo "nativepipe: tracked compositor is alive but both ports are not ready" >&2
                echo "nativepipe: inspect $LOG or stop PID $(cat "$PID") before retrying" >&2
                exit 1
              fi
              if listening; then
                echo 'nativepipe: ports 1025/1026 belong to an untracked process' >&2
                echo "nativepipe: stop it or remove stale $PID after verifying ownership" >&2
                exit 1
              fi
              rm -f "$ENV" "$PID"
              env XDG_RUNTIME_DIR="$RUNTIME" nohup "$COMPOSITOR" >"$LOG" 2>&1 &
              compositor_pid=$!
              printf '%s\\n' "$compositor_pid" >"$PID.tmp"
              mv "$PID.tmp" "$PID"
              attempts=0
              while [ "$attempts" -lt 25 ]; do
                owned_compositor && [ -f "$ENV" ] && break
                kill -0 "$compositor_pid" 2>/dev/null || break
                attempts=$((attempts + 1))
                sleep 0.2
              done
            fi
            if ! owned_compositor; then
              echo "nativepipe: compositor failed to listen on 127.0.0.1:1025/1026" >&2
              tail -n 40 "$LOG" 2>/dev/null >&2 || true
              rm -f "$PID"
              exit 1
            fi
            if [ ! -f "$ENV" ]; then
              echo "nativepipe: compositor is up but WAYLAND_DISPLAY is unknown" >&2
              tail -n 40 "$LOG" 2>/dev/null >&2 || true
              exit 1
            fi
            . "$ENV"
            echo "ready display=$WAYLAND_DISPLAY runtime=$XDG_RUNTIME_DIR"
            """
    }

    private struct SSHResult {
        var status: Int32
        var stdout: Data
        var stderr: Data
    }

    private static func runSSH(
        destination: String,
        sshArguments: [String],
        remoteCommand: String,
        allocateTTY: Bool,
        inheritStdio: Bool
    ) throws -> SSHResult {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/ssh")
        var args: [String] = []
        if allocateTTY { args.append("-t") }
        args.append(contentsOf: ["-o", "BatchMode=yes", "-o", "ConnectTimeout=15"])
        args.append(contentsOf: sshArguments)
        args.append(destination)
        args.append(remoteCommand)
        process.arguments = args

        let out = Pipe()
        let err = Pipe()
        if inheritStdio {
            process.standardInput = FileHandle.standardInput
            process.standardOutput = FileHandle.standardOutput
            process.standardError = FileHandle.standardError
        } else {
            process.standardInput = FileHandle.nullDevice
            process.standardOutput = out
            process.standardError = err
        }
        try process.run()
        process.waitUntilExit()
        return SSHResult(
            status: process.terminationStatus,
            stdout: inheritStdio ? Data() : out.fileHandleForReading.readDataToEndOfFile(),
            stderr: inheritStdio ? Data() : err.fileHandleForReading.readDataToEndOfFile())
    }

    private static func shellEscape(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private static func isPortFree(_ port: UInt16) -> Bool {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return false }
        defer { Darwin.close(fd) }
        var reuse: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, socklen_t(MemoryLayout.size(ofValue: reuse)))
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr = in_addr(s_addr: inet_addr("127.0.0.1"))
        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        return bindResult == 0
    }

    private static func reserveEphemeralPort() throws -> UInt16 {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else {
            throw CLIError.invalidUsage("socket() failed: \(errno)")
        }
        defer { Darwin.close(fd) }
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = 0
        addr.sin_addr = in_addr(s_addr: inet_addr("127.0.0.1"))
        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindResult == 0 else {
            throw CLIError.invalidUsage("bind ephemeral failed: \(errno)")
        }
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        let got = withUnsafeMutablePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                getsockname(fd, $0, &length)
            }
        }
        guard got == 0 else {
            throw CLIError.invalidUsage("getsockname failed: \(errno)")
        }
        return UInt16(bigEndian: addr.sin_port)
    }
}
