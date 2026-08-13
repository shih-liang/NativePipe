import Foundation

/// Host → guest RPC on the control channel. Wire encoding is binary
/// (`ControlWire`); these types are the Swift API only.
public struct Request: Sendable {
    public var id: UInt64
    public var call: Call

    public init(id: UInt64, call: Call) {
        self.id = id
        self.call = call
    }

    public enum Call: Sendable {
        /// Handshake. The guest answers with `.hello`, which is how the host
        /// learns that guestd is alive and which capabilities the rootfs offers.
        case hello(hostVersion: String)

        /// A serial console has no TIOCSWINSZ path, so window resizes are
        /// carried out-of-band and applied to the console tty by guestd.
        case resizeConsole(cols: Int, rows: Int)

        /// Launch a program in the guest. Fire-and-forget: returns a pid.
        /// `Stage 2` wires the resulting Wayland surfaces back to NSWindows.
        case launch(spec: LaunchSpec)

        /// Run a command to completion and return status + captured output.
        case run(spec: LaunchSpec)

        /// Read a guest file (raw bytes) or list one directory level.
        case readPath(path: String)

        /// `lstat` a guest path: type, permissions, owner, size, mtime.
        case statPath(path: String)

        /// Ask the guest to shut down cleanly (guestd calls into the distro init).
        case shutdown

        /// Round-trip liveness probe.
        case ping

        /// Create `username` if missing. If `oldUsername` is set, rename that
        /// account to `username` instead (usermod). Does not set a password.
        case setUser(username: String, oldUsername: String?)

        /// Set the password for an existing guest account (`chpasswd`).
        case setPassword(username: String, password: String)
    }
}

public struct LaunchSpec: Sendable {
    public var executable: String
    public var arguments: [String]
    public var environment: [String: String]
    public var workingDirectory: String?
    /// Bytes written to the child's stdin, then EOF. Used by `run` (ignored by `launch`).
    public var stdin: String?

    public init(
        executable: String,
        arguments: [String] = [],
        environment: [String: String] = [:],
        workingDirectory: String? = nil,
        stdin: String? = nil
    ) {
        self.executable = executable
        self.arguments = arguments
        self.environment = environment
        self.workingDirectory = workingDirectory
        self.stdin = stdin
    }
}

/// `dirent.d_type` values from Linux (may be `.unknown` on some filesystems).
public enum DirEntryType: UInt8, Sendable {
    case unknown = 0
    case fifo = 1
    case character = 2
    case directory = 4
    case block = 6
    case regular = 8
    case symlink = 10
    case socket = 12
    case whiteout = 14
}

/// One name in a directory listing. Directories are not walked.
/// `fileType` is `readdir`/`d_type` only; permissions and size need `statPath`.
public struct DirEntry: Sendable {
    public var name: String
    /// Linux `dirent.d_type`; `.unknown` if the filesystem did not fill it.
    public var fileType: DirEntryType
    /// This name continues the previous entry (split inside a name).
    public var continuesName: Bool
    /// This name is incomplete; the next chunk continues it.
    public var nameIncomplete: Bool

    public init(
        name: String, fileType: DirEntryType = .unknown, continuesName: Bool = false,
        nameIncomplete: Bool = false
    ) {
        self.name = name
        self.fileType = fileType
        self.continuesName = continuesName
        self.nameIncomplete = nameIncomplete
    }

    public var isDirectory: Bool { fileType == .directory }
    public var isRegular: Bool { fileType == .regular }
    public var isSymlink: Bool { fileType == .symlink }
}

/// `lstat` of a guest path, from `statPath`.
public struct PathStat: Sendable {
    public var path: String
    /// `stat.st_mode` (type bits + permission bits).
    public var mode: UInt32
    public var uid: UInt32
    public var gid: UInt32
    public var size: UInt64
    /// Modification time, Unix seconds.
    public var mtime: Int64

    public init(path: String, mode: UInt32, uid: UInt32, gid: UInt32, size: UInt64, mtime: Int64) {
        self.path = path
        self.mode = mode
        self.uid = uid
        self.gid = gid
        self.size = size
        self.mtime = mtime
    }

    public var isDirectory: Bool { (mode & 0o170000) == 0o040000 }
    public var isRegular: Bool { (mode & 0o170000) == 0o100000 }
    public var isSymlink: Bool { (mode & 0o170000) == 0o120000 }
    public var permissions: UInt32 { mode & 0o7777 }
}

/// Guest path payload returned by `readPath`.
///
/// A file fills `data` with the raw bytes. A directory fills `entries` with
/// the immediate children (one level, no recursion) — name + `d_type`. Large
/// directories arrive in chunks split between names when possible;
/// `GuestControlChannel.readPath` continues until the listing is complete,
/// or sends `NPCL` if the caller cancels. Use `statPath` for permissions.
public struct PathContents: Sendable {
    public var path: String
    public var isDirectory: Bool
    public var entries: [DirEntry]
    public var data: Data

    public init(path: String, isDirectory: Bool, entries: [DirEntry] = [], data: Data = Data()) {
        self.path = path
        self.isDirectory = isDirectory
        self.entries = entries
        self.data = data
    }
}

public struct Response: Sendable {
    public var id: UInt64
    public var result: Result

    public init(id: UInt64, result: Result) {
        self.id = id
        self.result = result
    }

    public enum Result: Sendable {
        case hello(info: GuestInfo)
        case ok
        case launched(pid: Int32)
        case ran(status: Int32, stdout: String, stderr: String)
        case pathContents(PathContents)
        case pathStat(PathStat)
        case failure(code: Int32, message: String)
    }
}

/// What guestd reports about the distro it found itself in. The host uses this
/// to decide which integrations to light up — there is no assumption that the
/// rootfs is any particular distribution.
public struct GuestInfo: Sendable {
    public var agentVersion: String
    public var kernelRelease: String
    public var distroName: String
    public var distroVersion: String
    public var initSystem: String
    public var capabilities: [String]

    public init(
        agentVersion: String,
        kernelRelease: String,
        distroName: String,
        distroVersion: String,
        initSystem: String,
        capabilities: [String]
    ) {
        self.agentVersion = agentVersion
        self.kernelRelease = kernelRelease
        self.distroName = distroName
        self.distroVersion = distroVersion
        self.initSystem = initSystem
        self.capabilities = capabilities
    }
}

/// Unsolicited guest → host notifications (binary events on the control channel).
public enum Event: Sendable {
    /// guestd finished bootstrapping and the NativePipe runtime is up.
    case runtimeReady(info: GuestInfo)
    /// A process started via `.launch` exited.
    case processExited(pid: Int32, status: Int32)
    /// Free-form diagnostic line, surfaced in the host's log window.
    case log(level: String, message: String)
}

/// Capability strings a guest may advertise in `GuestInfo.capabilities`.
public enum GuestCapability {
    public static let consoleResize = "console.resize"
    public static let launch = "process.launch"
    public static let wayland = "display.wayland"
    public static let clipboard = "integration.clipboard"
    public static let portal = "integration.portal"
    public static let credentials = "account.credentials"
    public static let run = "process.run"
    public static let readPath = "fs.read"
    public static let statPath = "fs.stat"
}
