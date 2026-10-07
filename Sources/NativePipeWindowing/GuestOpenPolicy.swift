import Foundation
import NativePipeProtocol
import NativePipeStrings
import UniformTypeIdentifiers

/// Validates syntax and current share authorization. The user, rather than a
/// scheme or file-type allowlist, decides whether the Mac may open the item.
public enum GuestOpenPolicy {
    public struct SharedFolder: Equatable, Sendable {
        public var tag: String
        public var hostRoot: URL
        public init(tag: String, hostRoot: URL) { self.tag = tag; self.hostRoot = hostRoot }
    }

    public enum Decision: Equatable {
        case openURL(URL)
        case receiveFile(path: String, name: String, share: SharedFolder?)
        case refuse(String)
    }

    public static let guestShareMount = "/mnt/linportal"

    public static func decide(_ request: HostOpenWire.Request, shares: [SharedFolder]) -> Decision {
        guard !request.value.utf8.contains(where: { $0 < 0x20 || $0 == 0x7f }),
              request.value.utf8.count <= HostOpenWire.maximumPayload else {
            return .refuse(NPText("That request was not understood."))
        }
        switch request.kind {
        case .url:
            guard let components = URLComponents(string: request.value),
                  let scheme = components.scheme, !scheme.isEmpty, let url = components.url,
                  !["http", "https"].contains(scheme.lowercased()) || components.host?.isEmpty == false else {
                return .refuse(NPText("That link is not valid."))
            }
            if scheme.lowercased() == "file" {
                // A file URL is a guest file request, even if a caller bypasses
                // the CLI's normalization. Never treat it as a Mac-local path.
                guard components.host == nil || components.host == "" || components.host == "localhost",
                      components.user == nil, components.password == nil,
                      components.query == nil, components.fragment == nil else {
                    return .refuse(NPText("Use a path on the connected Linux machine to receive files."))
                }
                return decide(.init(kind: .file, value: url.path), shares: shares)
            }
            return .openURL(url)
        case .file:
            let parts = request.value.split(separator: "/")
            guard request.value.hasPrefix("/"), !parts.isEmpty, request.value.utf8.count <= 4095,
                  !parts.contains("."), !parts.contains(".."), let name = parts.last else {
                return .refuse(NPText("That file path is not valid."))
            }
            let path = "/" + parts.joined(separator: "/")
            var share: SharedFolder?
            if path == guestShareMount || path.hasPrefix(guestShareMount + "/") {
                let relative = path.dropFirst(guestShareMount.count).split(separator: "/")
                guard let tag = relative.first, let current = shares.first(where: { $0.tag == tag }) else {
                    return .refuse(NPText("That shared folder is no longer available."))
                }
                share = current
            }
            // Use the user-level transport for private copies or versioned
            // range reads. Never reopen a guest-mutable Mac shared path.
            return .receiveFile(path: path, name: String(name), share: share)
        }
    }

    /// Conceal credentials in the approval UI, while retaining the exact URL
    /// for LaunchServices. The origin and attachment parameters remain visible.
    public static func displayURL(_ url: URL) -> String {
        guard var parts = URLComponents(url: url, resolvingAgainstBaseURL: false) else { return url.absoluteString }
        if parts.user != nil { parts.user = "…" }
        if parts.password != nil { parts.password = "…" }
        return parts.string ?? url.absoluteString
    }

    public static func safeFileName(_ name: String) -> String {
        let safe = String(name.unicodeScalars.filter { !CharacterSet.controlCharacters.contains($0) && $0 != "/" && $0 != ":" })
        return safe.isEmpty || safe == "." || safe == ".." ? "Received Item" : safe
    }

    /// This is a disclosure for an extra per-item execution confirmation, not
    /// a transfer filter. Unknown types still require the ordinary Open approval.
    public static func mayExecute(_ file: URL) throws -> Bool {
        let info = try file.resourceValues(forKeys: [.isExecutableKey, .contentTypeKey, .isPackageKey, .isDirectoryKey])
        if info.isPackage == true { return true }
        if info.isDirectory == true { return false }
        if info.isExecutable == true { return true }
        if let type = info.contentType, type.conforms(to: .executable) || type.conforms(to: .script)
            || type.conforms(to: .application) { return true }
        if let handle = try? FileHandle(forReadingFrom: file) {
            defer { try? handle.close() }
            let header = try handle.read(upToCount: 4) ?? Data()
            if header.starts(with: [0x23, 0x21]) || header.starts(with: [0x7f, 0x45, 0x4c, 0x46])
                || header.starts(with: [0x4d, 0x5a]) || [[0xcf, 0xfa, 0xed, 0xfe], [0xce, 0xfa, 0xed, 0xfe],
                    [0xfe, 0xed, 0xfa, 0xcf], [0xfe, 0xed, 0xfa, 0xce], [0xca, 0xfe, 0xba, 0xbe]].contains(Array(header)) { return true }
        }
        return ["pkg", "mpkg", "dmg", "iso", "command", "terminal", "workflow", "action",
                "scpt", "scptd", "applescript", "sh", "bash", "zsh", "fish", "py", "pl", "rb",
                "jar", "exe", "msi", "bat", "cmd", "ps1", "webloc", "url", "mobileconfig",
                "prefpane", "saver", "plugin", "xpc", "appex"].contains(file.pathExtension.lowercased())
    }
}
