import Foundation

/// Named file transfer over vsock (bootstrap, provision extras, self-update).
///
/// Guest → host request:
///   `"NPAG"` | wire(u8=1) | flags(u8=0)
///          | name_len(u16 LE) | name bytes
///          | ver_len(u16 LE) | ver bytes
///
/// Host → guest response:
///   `"NPAG"` | wire(u8=1) | status(u8: 0=file, 1=uptodate, 2=notfound, 3=force)
///          | ver_len(u16 LE) | ver bytes
///          | if status==0 or 3: payload_len(u64 LE) + file bytes
///
/// `force` is a host-initiated instruction on an already-open agent vsock:
/// the guest installs the payload without comparing versions.
public enum AgentWire {
    public static let magic = Data("NPAG".utf8)
    public static let version: UInt8 = 1
    public static let statusFile: UInt8 = 0
    public static let statusUptodate: UInt8 = 1
    public static let statusNotFound: UInt8 = 2
    /// Host command: install this payload now, ignore version.
    public static let statusForce: UInt8 = 3
    public static let maxNameLength = 256
    public static let maxVersionLength = 256
    public static let maxPayloadLength: UInt64 = 512 * 1024 * 1024
    public static let guestdName = "nativepipe-guestd"

    public struct Request: Equatable {
        public var name: String
        public var guestVersion: String

        public init(name: String, guestVersion: String = "") {
            self.name = name
            self.guestVersion = guestVersion
        }
    }

    public enum Response: Equatable {
        case file(hostVersion: String, payload: Data)
        case uptodate(hostVersion: String)
        case notFound(hostVersion: String)
        case force(hostVersion: String, payload: Data)

        var payload: Data? {
            switch self {
            case .file(_, let data), .force(_, let data):
                return data
            case .uptodate, .notFound:
                return nil
            }
        }
    }

    public enum Failure: LocalizedError {
        case badMagic
        case unsupportedWire
        case nameTooLong
        case versionTooLong
        case truncated
        case payloadTooLarge

        public var errorDescription: String? {
            switch self {
            case .badMagic: return "agent wire: bad magic"
            case .unsupportedWire: return "agent wire: unsupported version"
            case .nameTooLong: return "agent wire: name too long"
            case .versionTooLong: return "agent wire: version too long"
            case .truncated: return "agent wire: truncated frame"
            case .payloadTooLarge: return "agent wire: payload too large"
            }
        }
    }

    public static func encodeRequest(_ request: Request) throws -> Data {
        let name = Data(request.name.utf8)
        let ver = Data(request.guestVersion.utf8)
        guard name.count <= maxNameLength else { throw Failure.nameTooLong }
        guard ver.count <= maxVersionLength else { throw Failure.versionTooLong }
        var out = Data()
        out.append(magic)
        out.append(version)
        out.append(0) // flags
        out.append(contentsOf: u16le(UInt16(name.count)))
        out.append(name)
        out.append(contentsOf: u16le(UInt16(ver.count)))
        out.append(ver)
        return out
    }

    public static func decodeRequest(from data: Data) throws -> Request {
        guard data.count >= 8 else { throw Failure.truncated }
        guard data.prefix(4) == magic else { throw Failure.badMagic }
        guard data[4] == version else { throw Failure.unsupportedWire }
        let nameLen = Int(u16le(data, at: 6))
        guard nameLen <= maxNameLength else { throw Failure.nameTooLong }
        guard data.count >= 8 + nameLen + 2 else { throw Failure.truncated }
        let name = String(data: data.subdata(in: 8..<(8 + nameLen)), encoding: .utf8) ?? ""
        let verOff = 8 + nameLen
        let verLen = Int(u16le(data, at: verOff))
        guard verLen <= maxVersionLength else { throw Failure.versionTooLong }
        guard data.count >= verOff + 2 + verLen else { throw Failure.truncated }
        let ver = String(
            data: data.subdata(in: (verOff + 2)..<(verOff + 2 + verLen)),
            encoding: .utf8) ?? ""
        return Request(name: name, guestVersion: ver)
    }

    public static func encodeResponseHeader(_ response: Response) throws -> Data {
        let hostVersion: String
        let status: UInt8
        switch response {
        case .uptodate(let v):
            hostVersion = v
            status = statusUptodate
        case .notFound(let v):
            hostVersion = v
            status = statusNotFound
        case .file(let v, _):
            hostVersion = v
            status = statusFile
        case .force(let v, _):
            hostVersion = v
            status = statusForce
        }
        let ver = Data(hostVersion.utf8)
        guard ver.count <= maxVersionLength else { throw Failure.versionTooLong }
        var out = Data()
        out.append(magic)
        out.append(version)
        out.append(status)
        out.append(contentsOf: u16le(UInt16(ver.count)))
        out.append(ver)
        if let payload = response.payload {
            guard UInt64(payload.count) <= maxPayloadLength else { throw Failure.payloadTooLarge }
            out.append(contentsOf: u64le(UInt64(payload.count)))
        }
        return out
    }

    public static func encodeFullResponse(_ response: Response) throws -> Data {
        var out = try encodeResponseHeader(response)
        if let payload = response.payload {
            out.append(payload)
        }
        return out
    }

    /// Compare dotted numeric versions; non-numeric tails compare lexicographically.
    public static func isNewer(host: String, than guest: String) -> Bool {
        let g = guest.trimmingCharacters(in: .whitespacesAndNewlines)
        if g.isEmpty { return true }
        let h = host.trimmingCharacters(in: .whitespacesAndNewlines)
        if h.isEmpty { return false }
        if h == g { return false }
        let hp = h.split(separator: ".").map(String.init)
        let gp = g.split(separator: ".").map(String.init)
        let n = max(hp.count, gp.count)
        for i in 0..<n {
            let a = i < hp.count ? hp[i] : "0"
            let b = i < gp.count ? gp[i] : "0"
            if let ai = Int(a), let bi = Int(b) {
                if ai != bi { return ai > bi }
            } else if a != b {
                return a > b
            }
        }
        return false
    }

    private static func u16le(_ v: UInt16) -> [UInt8] {
        [UInt8(v & 0xff), UInt8((v >> 8) & 0xff)]
    }

    private static func u64le(_ v: UInt64) -> [UInt8] {
        (0..<8).map { UInt8((v >> (8 * $0)) & 0xff) }
    }

    private static func u16le(_ data: Data, at offset: Int) -> UInt16 {
        UInt16(data[offset]) | (UInt16(data[offset + 1]) << 8)
    }
}
