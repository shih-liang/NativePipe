import NativePipeStrings
import Foundation
import Security
import CryptoKit
import LocalAuthentication

/// Secrets stay in Keychain, separate from plist connection records.
public struct SSHCredentialStore {
    public struct Entry: Identifiable {
        public let id: String
        public var title: String { id == SSHCredentialStore.loginPasswordAccount ? NPText("Login password") : id }
    }
    // A typed password belongs to the connection, not to guessed OpenSSH text.
    private static let loginPasswordAccount = "nativepipe:login-password"
    private let service: String
    private let group: String?
    private let trustedApplications: [URL]
    public init(connection: String, accessGroup: String? = nil, trustedApplications: [URL] = []) {
        service = "com.nativepipe.ssh." + SHA256.hash(data: Data(connection.utf8))
            .map { String(format: "%02x", $0) }.joined()
        group = accessGroup
        self.trustedApplications = trustedApplications
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
        var result: CFTypeRef?
        let status = try perform { query, _ in
            var query = query
            query[kSecReturnAttributes as String] = true
            query[kSecMatchLimit as String] = kSecMatchLimitAll
            return SecItemCopyMatching(query as CFDictionary, &result)
        }
        if status == errSecItemNotFound { return [] }
        try check(status)
        return (result as? [[String: Any]] ?? []).compactMap {
            ($0[kSecAttrAccount as String] as? String).map { Entry(id: $0) }
        }
    }
    public func read(_ prompt: String) throws -> String? {
        var result: CFTypeRef?
        let status = try perform { query, _ in
            var query = query
            query[kSecAttrAccount as String] = prompt
            query[kSecReturnData as String] = true
            return SecItemCopyMatching(query as CFDictionary, &result)
        }
        if status == errSecItemNotFound { return nil }
        try check(status)
        return (result as? Data).flatMap { String(data: $0, encoding: .utf8) }
    }
    public func save(_ secret: String, prompt: String) throws {
        if prompt == Self.loginPasswordAccount { try Self.validateLoginPassword(secret) }
        try check(try perform { query, fallback in
            var query = query
            query[kSecAttrAccount as String] = prompt
            let values = [kSecValueData as String: Data(secret.utf8)] as [String: Any]
            // Updating a secret must preserve the original access policy.
            // A nested helper may not inspect another owner's bundle on disk.
            let status = SecItemUpdate(query as CFDictionary, values as CFDictionary)
            guard status == errSecItemNotFound else { return status }
            query.merge(values) { _, new in new }
            if fallback { query[kSecAttrAccess as String] = try applicationAccess() }
            query[kSecAttrLabel as String] = NPText("NativePipe SSH — %@", prompt == Self.loginPasswordAccount ? NPText("Login password") : prompt)
            if !fallback { query[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly }
            return SecItemAdd(query as CFDictionary, nil)
        })
    }
    public func remove(_ prompt: String? = nil) throws {
        let status = try perform { query, _ in
            var query = query
            if let prompt { query[kSecAttrAccount as String] = prompt }
            return SecItemDelete(query as CFDictionary)
        }
        if status != errSecItemNotFound { try check(status) }
    }
    public func saveLoginPassword(_ secret: String) throws {
        try save(secret, prompt: Self.loginPasswordAccount)
    }
    /// Checks only the typed account's presence; callers never receive its data.
    public func hasLoginPassword() throws -> Bool {
        let status = try perform { query, _ in
            var query = query
            query[kSecAttrAccount as String] = Self.loginPasswordAccount
            let context = LAContext()
            context.interactionNotAllowed = true
            query[kSecUseAuthenticationContext as String] = context
            return SecItemCopyMatching(query as CFDictionary, nil)
        }
        if status == errSecItemNotFound { return false }
        try check(status)
        return true
    }
    public func removeLoginPassword() throws {
        try remove(Self.loginPasswordAccount)
    }
    static func validateLoginPassword(_ secret: String) throws {
        guard !secret.isEmpty, !secret.unicodeScalars.contains(where: { [0, 10, 13].contains($0.value) }) else {
            throw RemoteError.message(NPText("Enter a non-empty password without line breaks or null characters."))
        }
    }
    func cachedResponse(prompt: String, confirming: Bool, directory: String?) throws -> String? {
        try Self.cachedResponse(prompt: prompt, confirming: confirming, directory: directory,
            readLoginPassword: { try read(Self.loginPasswordAccount) }, readPrompt: read)
    }
    /// The same gate covers typed and legacy passwords: rejection must lead to
    /// a prompt, not a second automatic attempt with an older cached value.
    static func cachedResponse(prompt: String, confirming: Bool, directory: String?,
                               readLoginPassword: () throws -> String?,
                               readPrompt: (String) throws -> String?) rethrows -> String? {
        guard !confirming, mayRemember(prompt) else { return nil }
        let loginPassword = isLoginPasswordPrompt(prompt)
        guard claimCachedAttempt(prompt: loginPassword ? loginPasswordAccount : prompt, directory: directory) else { return nil }
        if loginPassword, let saved = try readLoginPassword() { return saved }
        return try readPrompt(prompt)
    }
    /// Keep existing Data Protection items when that entitlement is available.
    /// Development signing may only grant the file container, not Keychain.
    /// In that case the login Keychain trusts explicit signed owners, never all apps.
    private func perform(_ action: ([String: Any], Bool) throws -> OSStatus) throws -> OSStatus {
        let status = try action(base, false)
        guard status == errSecMissingEntitlement, group != nil, !trustedApplications.isEmpty else { return status }
        var query = base
        query.removeValue(forKey: kSecAttrAccessGroup as String)
        query.removeValue(forKey: kSecUseDataProtectionKeychain as String)
        return try action(query, true)
    }
    private func applicationAccess() throws -> SecAccess {
        var trusted: [SecTrustedApplication] = []
        for url in trustedApplications {
            var application: SecTrustedApplication?
            try check(SecTrustedApplicationCreateFromPath(url.path, &application))
            guard let application else { throw RemoteError.message(NPText("The SSH password owner could not be authorized.")) }
            trusted.append(application)
        }
        var access: SecAccess?
        try check(SecAccessCreate(NPText("NativePipe SSH") as CFString, trusted as CFArray, &access))
        guard let access else { throw RemoteError.message(NPText("The SSH password access policy could not be created.")) }
        return access
    }
    private func check(_ status: OSStatus) throws {
        guard status == errSecSuccess else {
            throw RemoteError.message(NPText("Keychain: %@", SecCopyErrorMessageString(status, nil) as String? ?? String(status)))
        }
    }

    static func mayRemember(_ prompt: String) -> Bool {
        let value = prompt.lowercased().trimmingCharacters(in: .whitespacesAndNewlines)
        return isLoginPasswordPrompt(prompt) || (value.hasPrefix("enter passphrase for key ") && value.hasSuffix(":"))
    }
    static func isLoginPasswordPrompt(_ prompt: String) -> Bool {
        let value = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        let suffix = "'s password:"
        guard value.hasSuffix(suffix) else { return false }
        let account = value.dropLast(suffix.count)
        let parts = account.split(separator: "@", omittingEmptySubsequences: false)
        // A bare Password: can be keyboard-interactive/MFA. Only the explicit
        // client-shaped user@host prompt may consume the connection password.
        // Askpass text cannot distinguish a server that deliberately imitates it.
        return parts.count == 2 && parts.allSatisfy { !$0.isEmpty }
            && !account.unicodeScalars.contains(where: { CharacterSet.whitespacesAndNewlines.union(.controlCharacters).contains($0) })
    }

    public static func makeAttemptDirectory(environment: [String: String]) throws -> URL {
        let files = FileManager.default
        let root: URL
        if let group = environment["NATIVEPIPE_KEYCHAIN_GROUP"] {
            // The manager's SFTP and RemoteHost askpass have different sandboxes.
            // Their retry markers must be shared, just like their Keychain items.
            guard let shared = files.containerURL(forSecurityApplicationGroupIdentifier: group) else {
                throw RemoteError.message(NPText("The SSH credential group is unavailable."))
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
