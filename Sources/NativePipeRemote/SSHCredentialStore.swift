import Foundation
import Security
import CryptoKit

/// Secrets stay in Keychain, separate from plist connection records.
public struct SSHCredentialStore {
    public struct Entry: Identifiable {
        public let id: String
        public var title: String { id }
    }
    private let service: String
    private let group: String?
    public init(connection: String, accessGroup: String? = nil) {
        service = "com.nativepipe.ssh." + SHA256.hash(data: Data(connection.utf8))
            .map { String(format: "%02x", $0) }.joined()
        group = accessGroup
    }
    private var base: [String: Any] {
        var value: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
        ]
        if let group {
            value[kSecAttrAccessGroup as String] = group
            value[kSecUseDataProtectionKeychain as String] = true
        }
        return value
    }
    public func entries() throws -> [Entry] {
        var query = base
        query[kSecReturnAttributes as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitAll
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        if status == errSecItemNotFound { return [] }
        try check(status)
        return (result as? [[String: Any]] ?? []).compactMap {
            ($0[kSecAttrAccount as String] as? String).map { Entry(id: $0) }
        }
    }
    public func read(_ prompt: String) throws -> String? {
        var query = base
        query[kSecAttrAccount as String] = prompt
        query[kSecReturnData as String] = true
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        if status == errSecItemNotFound { return nil }
        try check(status)
        return (result as? Data).flatMap { String(data: $0, encoding: .utf8) }
    }
    public func save(_ secret: String, prompt: String) throws {
        var query = base
        query[kSecAttrAccount as String] = prompt
        let values = [kSecValueData as String: Data(secret.utf8)] as [String: Any]
        let status = SecItemUpdate(query as CFDictionary, values as CFDictionary)
        if status == errSecItemNotFound {
            query.merge(values) { _, new in new }
            query[kSecAttrLabel as String] = "NativePipe SSH — " + prompt
            query[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
            try check(SecItemAdd(query as CFDictionary, nil))
        } else { try check(status) }
    }
    public func remove(_ prompt: String? = nil) throws {
        var query = base
        if let prompt { query[kSecAttrAccount as String] = prompt }
        let status = SecItemDelete(query as CFDictionary)
        if status != errSecItemNotFound { try check(status) }
    }
    private func check(_ status: OSStatus) throws {
        guard status == errSecSuccess else {
            throw RemoteError.message("Keychain: " + (SecCopyErrorMessageString(status, nil) as String? ?? String(status)))
        }
    }

    static func mayRemember(_ prompt: String) -> Bool {
        let value = prompt.lowercased().trimmingCharacters(in: .whitespacesAndNewlines)
        return value.hasSuffix("password:") || value.hasPrefix("enter passphrase for key ")
    }

    static func makeAttemptDirectory(environment: [String: String]) throws -> URL {
        let files = FileManager.default
        let root: URL
        if let group = environment["NATIVEPIPE_KEYCHAIN_GROUP"] {
            // The manager's SFTP and RemoteHost askpass have different sandboxes.
            // Their retry markers must be shared, just like their Keychain items.
            guard let shared = files.containerURL(forSecurityApplicationGroupIdentifier: group) else {
                throw RemoteError.message("The SSH credential group is unavailable.")
            }
            root = shared
        } else { root = files.temporaryDirectory }
        let directory = root.appendingPathComponent("nativepipe-auth-" + UUID().uuidString)
        try files.createDirectory(at: directory, withIntermediateDirectories: false,
                                  attributes: [.posixPermissions: 0o700])
        return directory
    }

    /// Askpass is a new process on every prompt. An empty marker records only
    /// that this session already tried the cached value, so a rejected password
    /// opens the prompt again instead of being replayed indefinitely.
    static func claimCachedAttempt(prompt: String, directory: String?) -> Bool {
        guard let directory else { return false }
        let hash = SHA256.hash(data: Data(prompt.utf8)).map { String(format: "%02x", $0) }.joined()
        let url = URL(fileURLWithPath: directory).appendingPathComponent(hash)
        let fd = open(url.path, O_WRONLY | O_CREAT | O_EXCL, 0o600)
        if fd < 0 { return false }
        close(fd)
        return true
    }
}
