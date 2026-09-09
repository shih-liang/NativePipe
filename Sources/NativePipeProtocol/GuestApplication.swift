import Foundation

/// Metadata from the ordinary-user compositor. Only the desktop ID is sent
/// back when launching: macOS never parses or executes a cached Exec command.
public struct GuestApplication: Identifiable, Codable, Sendable, Equatable {
    public let id, name, executable: String
    public let comment, startupWMClass, iconName: String?
    public var iconData: Data?

    public init(id: String, name: String, comment: String? = nil, executable: String = "",
                startupWMClass: String? = nil, iconName: String? = nil, iconData: Data? = nil) {
        self.id = id; self.name = name; self.comment = comment; self.executable = executable
        self.startupWMClass = startupWMClass; self.iconName = iconName; self.iconData = iconData
    }
    public func matches(applicationID: String) -> Bool {
        WindowApplicationIdentity.matches(applicationID, candidates: [
            id, URL(fileURLWithPath: executable).lastPathComponent, startupWMClass
        ])
    }
    public var fallbackSystemImageName: String {
        iconName == "utilities-terminal" ? "terminal.fill" : "app.fill"
    }
}

public enum ApplicationAction: UInt32, Sendable { case list, launch, icon, appearance }

/// NPAP replies inside the existing NPIP event stream. Catalogs are streamed
/// in batches of up to 32 metadata records, followed by an explicit completion.
/// Icons are separately requested 64-pixel PNG thumbnails, not original files.
public enum ApplicationReply: Sendable {
    case batch(UInt32, [GuestApplication])
    case launched(UInt32, Int32, String)
    case changed
    case end(UInt32, String)
    case icon(UInt32, Data)
    case exited(Int32, Int32)
    public static let magic = Data("NPAP".utf8)

    public static func decode(_ data: Data) throws -> Self {
        guard data.count >= 12, data.prefix(4) == magic,
              data[4] == 1, data[6] == 0, data[7] == 0 else { throw WindowWire.DecodeError.malformed }
        var r = Reader(data: data)
        let token = try r.number()
        guard [UInt8(3), 6].contains(data[5]) || token != 0 else { throw WindowWire.DecodeError.malformed }
        let reply: Self
        switch data[5] {
        case 1:
            let count = try r.number()
            guard token != 0, count <= 32 else { throw WindowWire.DecodeError.malformed }
            var apps: [GuestApplication] = []
            for _ in 0..<count {
                let id = try r.string(), name = try r.string(), comment = try r.string()
                let executable = try r.string(), wm = try r.string(), icon = try r.string()
                guard !id.isEmpty, !id.contains("/") else { throw WindowWire.DecodeError.malformed }
                apps.append(GuestApplication(id: id, name: name,
                    comment: comment.isEmpty ? nil : comment, executable: executable,
                    startupWMClass: wm.isEmpty ? nil : wm, iconName: icon.isEmpty ? nil : icon))
            }
            reply = .batch(token, apps)
        case 2: reply = try .launched(token, Int32(bitPattern: r.number()), r.string())
        case 3:
            guard token == 0 else { throw WindowWire.DecodeError.malformed }
            reply = .changed
        case 4: reply = try .end(token, r.string())
        case 5: reply = try .icon(token, r.bytes(limit: 65_536))
        case 6:
            guard token == 0 else { throw WindowWire.DecodeError.malformed }
            reply = try .exited(Int32(bitPattern: r.number()), Int32(bitPattern: r.number()))
        default: throw WindowWire.DecodeError.malformed
        }
        guard r.offset == data.count else { throw WindowWire.DecodeError.malformed }
        return reply
    }
    private struct Reader {
        let data: Data
        var offset = 8
        mutating func number() throws -> UInt32 {
            guard data.count - offset >= 4 else { throw WindowWire.DecodeError.truncated }
            defer { offset += 4 }
            return data[offset..<offset + 4].reversed().reduce(0) { ($0 << 8) | UInt32($1) }
        }
        mutating func bytes(limit: Int) throws -> Data {
            let count = Int(try number())
            guard count <= limit, count <= data.count - offset else { throw WindowWire.DecodeError.malformed }
            defer { offset += count }
            return Data(data[offset..<offset + count])
        }
        mutating func string() throws -> String {
            guard let text = String(data: try bytes(limit: 1_048_576), encoding: .utf8), !text.contains("\0")
            else { throw WindowWire.DecodeError.malformed }
            return text
        }
    }
}
