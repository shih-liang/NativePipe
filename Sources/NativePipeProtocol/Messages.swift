import Foundation

/// Every frame on the control channel is one of these three shapes.
public enum ControlMessage: Codable, Sendable {
    case request(Request)
    case response(Response)
    case event(Event)

    private enum CodingKeys: String, CodingKey { case type, body }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        switch try c.decode(String.self, forKey: .type) {
        case "request": self = .request(try c.decode(Request.self, forKey: .body))
        case "response": self = .response(try c.decode(Response.self, forKey: .body))
        case "event": self = .event(try c.decode(Event.self, forKey: .body))
        case let other:
            throw DecodingError.dataCorruptedError(
                forKey: .type, in: c, debugDescription: "unknown message type '\(other)'")
        }
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .request(let r):
            try c.encode("request", forKey: .type)
            try c.encode(r, forKey: .body)
        case .response(let r):
            try c.encode("response", forKey: .type)
            try c.encode(r, forKey: .body)
        case .event(let e):
            try c.encode("event", forKey: .type)
            try c.encode(e, forKey: .body)
        }
    }
}

public struct Request: Codable, Sendable {
    public var id: UInt64
    public var call: Call

    public init(id: UInt64, call: Call) {
        self.id = id
        self.call = call
    }

    public enum Call: Codable, Sendable {
        /// Handshake. The guest answers with `.hello`, which is how the host
        /// learns that guestd is alive and which capabilities the rootfs offers.
        case hello(hostVersion: String)

        /// A serial console has no TIOCSWINSZ path, so window resizes are
        /// carried out-of-band and applied to the console tty by guestd.
        case resizeConsole(cols: Int, rows: Int)

        /// Launch a program in the guest. `Stage 2` wires the resulting Wayland
        /// surfaces back to NSWindows; today it just runs and reports exit status.
        case launch(spec: LaunchSpec)

        /// Ask the guest to shut down cleanly (guestd calls into the distro init).
        case shutdown

        /// Round-trip liveness probe.
        case ping
    }
}

public struct LaunchSpec: Codable, Sendable {
    public var executable: String
    public var arguments: [String]
    public var environment: [String: String]
    public var workingDirectory: String?

    public init(
        executable: String,
        arguments: [String] = [],
        environment: [String: String] = [:],
        workingDirectory: String? = nil
    ) {
        self.executable = executable
        self.arguments = arguments
        self.environment = environment
        self.workingDirectory = workingDirectory
    }
}

public struct Response: Codable, Sendable {
    public var id: UInt64
    public var result: Result

    public init(id: UInt64, result: Result) {
        self.id = id
        self.result = result
    }

    public enum Result: Codable, Sendable {
        case hello(info: GuestInfo)
        case ok
        case launched(pid: Int32)
        case failure(code: Int32, message: String)
    }
}

/// What guestd reports about the distro it found itself in. The host uses this
/// to decide which integrations to light up — there is no assumption that the
/// rootfs is any particular distribution.
public struct GuestInfo: Codable, Sendable {
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

/// Unsolicited guest -> host notifications.
public enum Event: Codable, Sendable {
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
}
