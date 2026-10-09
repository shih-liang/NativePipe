@_exported import NativePipeNotifications
import Foundation
import CryptoKit
import Darwin
import NativePipeProtocol
import UserNotifications

/// Everything a guest sends is untrusted text: any program in the machine can
/// post notifications. These rules bound size and strip anything that is not
/// plain text before macOS sees it.
enum GuestNotificationPolicy {
    static let maximumTitleLength = GuestNotificationLimits.title
    static let maximumBodyLength = GuestNotificationLimits.body
    static let maximumAppNameLength = GuestNotificationLimits.appName
    static let maximumActionLabelLength = GuestNotificationLimits.actionLabel
    static let maximumActionKeyLength = GuestNotificationLimits.actionKey
    /// macOS shows only a few buttons; the click on the banner is "default".
    static let maximumActions = GuestNotificationLimits.actions
    static let maximumExpiry = GuestNotificationLimits.expiry
    static let defaultActionKey = GuestNotificationLimits.defaultActionKey

    static func content(
        for notification: Windowing.GuestNotification, machine: String
    ) -> GuestNotificationContent? {
        let title = plainText(notification.summary, limit: maximumTitleLength)
        let body = plainText(notification.body, limit: maximumBodyLength, keepNewlines: true)
        // A notification with nothing to read is not worth interrupting for.
        guard !title.isEmpty || !body.isEmpty else { return nil }
        let app = plainText(notification.appName, limit: maximumAppNameLength)
        var seen = Set<String>()
        let actions: [GuestNotificationContent.Action] = notification.actions.compactMap { action in
            let key = action.key
            let label = plainText(action.label, limit: maximumActionLabelLength)
            guard !key.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, key != defaultActionKey,
                  key != UNNotificationDefaultActionIdentifier, key != UNNotificationDismissActionIdentifier,
                  !key.unicodeScalars.contains(where: CharacterSet.controlCharacters.contains),
                  key.utf8.count <= maximumActionKeyLength,
                  !label.isEmpty, seen.insert(key).inserted else { return nil }
            return .init(key: key, label: label)
        }
        let expiry: TimeInterval? = notification.timeoutMilliseconds > 0
            ? min(Double(notification.timeoutMilliseconds) / 1000, maximumExpiry) : nil
        return GuestNotificationContent(
            title: title.isEmpty ? plainText(body, limit: maximumTitleLength) : title,
            subtitle: app.isEmpty ? machine : "\(machine) · \(app)",
            body: title.isEmpty ? "" : body,
            actions: Array(actions.prefix(maximumActions)),
            expiry: expiry,
            quiet: notification.urgency == .low)
    }

    /// Strips tags and entities (the spec allows a small HTML subset in bodies),
    /// control characters and runs of blanks, then bounds the length.
    static func plainText(_ raw: String, limit: Int, keepNewlines: Bool = false) -> String {
        var text = raw.replacingOccurrences(of: "\r\n", with: "\n")
        if text.contains("<") {
            text = text.replacingOccurrences(of: "<[^>]*>", with: "", options: .regularExpression)
        }
        for (entity, character) in [("&lt;", "<"), ("&gt;", ">"), ("&quot;", "\""),
                                    ("&apos;", "'"), ("&amp;", "&")] {
            text = text.replacingOccurrences(of: entity, with: character)
        }
        let cleaned = String(String.UnicodeScalarView(text.unicodeScalars.map { scalar in
            if scalar == "\n" || scalar == "\r" { return keepNewlines ? "\n" : " " }
            if scalar == "\t" { return " " }
            return CharacterSet.controlCharacters.contains(scalar) || scalar.properties.isDefaultIgnorableCodePoint
                ? " " : scalar
        }))
        var collapsed = cleaned
            .replacingOccurrences(of: "[ ]{2,}", with: " ", options: .regularExpression)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if keepNewlines {
            collapsed = collapsed.replacingOccurrences(of: "\n{3,}", with: "\n\n", options: .regularExpression)
        }
        guard collapsed.count > limit else { return collapsed }
        return String(collapsed.prefix(limit - 1)) + "…"
    }
}

/// A burst of a few requests is normal; a guest program making them
/// continuously is not, and must not be able to fill Notification Center, the
/// user's screen or their browser tabs.
struct GuestRequestRateLimiter {
    let capacity: Double
    let refillInterval: TimeInterval
    private var tokens: Double
    private var last: TimeInterval?

    init(capacity: Int = 5, refillInterval: TimeInterval = 2) {
        self.capacity = Double(capacity)
        self.refillInterval = refillInterval
        tokens = Double(capacity)
    }

    mutating func allow(now: TimeInterval) -> Bool {
        if let last { tokens = min(capacity, tokens + max(0, now - last) / refillInterval) }
        last = now
        guard tokens >= 1 else { return false }
        tokens -= 1
        return true
    }
}

typealias GuestNotificationRateLimiter = GuestRequestRateLimiter

/// The system center is asynchronous; tests can suspend authorization and add
/// independently to verify replacement and teardown while callbacks are late.
@MainActor
protocol GuestNotificationCenter: AnyObject {
    func authorize() async -> Bool
    func present(identifier: String, content: GuestNotificationContent) async throws
    func withdraw(identifier: String)
    var onResponse: ((String, String?) -> Bool)? { get set }
}

/// Shared VM/SSH notification owner. IDs include the machine, connection session
/// and replacement revision, so an old asynchronous add cannot overwrite a new
/// notification or deliver its action to another machine.
@MainActor
public final class GuestNotificationPresenter {
    private final class Entry {
        let identifier: String
        let revision: UInt64
        let actions: Set<String>
        var record: GuestNotificationHistoryRecord
        var task: Task<Void, Never>?
        init(identifier: String, revision: UInt64, actions: Set<String>, record: GuestNotificationHistoryRecord) {
            self.identifier = identifier; self.revision = revision; self.actions = actions
            self.record = record
        }
    }
    private let machine: String
    private let machineIdentity: String
    private let center: GuestNotificationCenter
    private let isEnabled: () -> Bool
    private let now: () -> TimeInterval
    private let send: (Windowing.HostCommand) -> Void
    private let archive: ((GuestNotificationHistoryRecord) -> Void)?
    private let initialLimiter: GuestNotificationRateLimiter
    private var limiter: GuestNotificationRateLimiter
    private var active: [UInt32: Entry] = [:]
    private var session = UUID()
    private var stopped = false
    static let maximumActive = 64

    /// Native macOS notifications require an actual app bundle. A bare CLI can
    /// still run its display without constructing an invalid system center.
    public convenience init(machine: String, identity: String,
                            responseDirectory: URL = FileManager.default.temporaryDirectory,
                            backend: GuestNotificationBackend? = nil,
                            isEnabled: @escaping () -> Bool = { true },
                            archive: ((GuestNotificationHistoryRecord) -> Void)? = nil,
                            send: @escaping (Windowing.HostCommand) -> Void) {
        let center: GuestNotificationCenter = Bundle.main.bundleURL.pathExtension == "app"
            && Bundle.main.bundleIdentifier != nil
            ? SystemGuestNotificationCenter(responseDirectory: responseDirectory, backend: backend)
            : UnavailableGuestNotificationCenter()
        self.init(machine: machine, identity: identity, center: center, isEnabled: isEnabled, archive: archive, send: send)
    }

    init(machine: String, identity: String = "test-machine", center: GuestNotificationCenter,
         isEnabled: @escaping () -> Bool = { true }, limiter: GuestNotificationRateLimiter = .init(),
         now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
         archive: ((GuestNotificationHistoryRecord) -> Void)? = nil,
         send: @escaping (Windowing.HostCommand) -> Void) {
        self.machine = machine
        machineIdentity = SHA256.hash(data: Data(identity.utf8)).map { String(format: "%02x", $0) }.joined()
        self.center = center; self.isEnabled = isEnabled; self.now = now; self.send = send
        self.archive = archive
        initialLimiter = limiter; self.limiter = limiter
        bindResponses()
    }

    var activeIdentifiers: Set<UInt32> { Set(active.keys) }
    func identifier(for id: UInt32) -> String? { active[id]?.identifier }

    private func bindResponses() {
        center.onResponse = { [weak self] identifier, action in
            self?.handleResponse(identifier: identifier, action: action) ?? false
        }
    }

    public func post(_ notification: Windowing.GuestNotification) {
        let id = notification.id
        guard !stopped, id != 0, notification.revision != 0 else { return }
        if let previous = active[id], previous.revision >= notification.revision { return }
        guard isEnabled(), limiter.allow(now: now()),
              let content = GuestNotificationPolicy.content(for: notification, machine: machine),
              active[id] != nil || active.count < Self.maximumActive else {
            close(id: id, revision: notification.revision)
            send(.notificationClosed(id: id, revision: notification.revision, reason: .undefined))
            return
        }
        remove(id: id)
        let identifier = "nativepipe.guest.\(machineIdentity).\(session.uuidString).\(id).\(notification.revision).\(UUID().uuidString)"
        let machineLabel = GuestNotificationPolicy.plainText(machine, limit: GuestNotificationPolicy.maximumTitleLength)
        let record = GuestNotificationHistoryRecord(id: identifier, machine: machineLabel.isEmpty ? "Guest" : machineLabel,
            application: GuestNotificationPolicy.plainText(notification.appName, limit: GuestNotificationPolicy.maximumAppNameLength),
            title: content.title, body: content.body, receivedAt: Date(), delivery: .pending)
        let entry = Entry(identifier: identifier, revision: notification.revision,
            actions: Set(content.actions.map(\.key) + [GuestNotificationPolicy.defaultActionKey]), record: record)
        active[id] = entry
        archive?(record)
        guard owns(entry, id: id) else { return }
        entry.task = Task { @MainActor [weak self, entry, center] in
            defer { entry.task = nil }
            let allowed = await center.authorize() // Fresh settings; denied is not cached forever.
            guard self?.owns(entry, id: id) == true else { return }
            guard !Task.isCancelled, self?.isEnabled() == true else { self?.reject(entry, id: id); return }
            guard allowed else {
                self?.updateDelivery(.notAuthorized, for: entry, id: id)
                self?.reject(entry, id: id)
                return
            }
            do { try await center.present(identifier: identifier, content: content) }
            catch {
                // This task may belong to an earlier replacement. Its failure
                // must never remove the current notification's record.
                if self?.owns(entry, id: id) == true {
                    self?.updateDelivery(.failed, for: entry, id: id)
                    self?.reject(entry, id: id)
                }
                else { center.withdraw(identifier: identifier) }
                return
            }
            guard self?.owns(entry, id: id) == true, !Task.isCancelled, self?.isEnabled() == true else {
                center.withdraw(identifier: identifier)
                if self?.owns(entry, id: id) == true { self?.reject(entry, id: id) }
                return
            }
            self?.updateDelivery(.delivered, for: entry, id: id)
            if let expiry = content.expiry {
                do { try await Task.sleep(for: .seconds(expiry)) }
                catch { return }
                guard let self, self.owns(entry, id: id), !Task.isCancelled else { return }
                self.remove(id: id)
                self.send(.notificationClosed(id: id, revision: entry.revision, reason: .expired))
            }
        }
    }

    private func owns(_ entry: Entry, id: UInt32) -> Bool { !stopped && active[id] === entry }
    private func updateDelivery(_ delivery: GuestNotificationDelivery, for entry: Entry, id: UInt32) {
        guard owns(entry, id: id) else { return }
        entry.record.delivery = delivery
        archive?(entry.record)
    }
    private func reject(_ entry: Entry, id: UInt32) {
        guard owns(entry, id: id) else { return }
        remove(id: id)
        send(.notificationClosed(id: id, revision: entry.revision, reason: .undefined))
    }

    /// Guest withdrawal is already acknowledged by the guest's D-Bus service.
    public func close(id: UInt32, revision: UInt64) {
        guard let entry = active[id], entry.revision <= revision else { return }
        remove(id: id)
    }

    private func remove(id: UInt32) {
        guard let entry = active.removeValue(forKey: id) else { return }
        let task = entry.task; entry.task = nil; task?.cancel()
        if entry.record.delivery == .pending {
            entry.record.delivery = .cancelled
            archive?(entry.record)
        }
        center.withdraw(identifier: entry.identifier)
    }

    public func refreshPreferences() {
        guard !stopped, !isEnabled() else { return }
        clear(notifyGuest: true)
    }

    public func resetBacklog() {
        guard !stopped else { return }
        clear(notifyGuest: true)
    }

    public func resetSession() {
        // The new handshake has a new ID namespace. Closing old guest IDs on
        // this transport could close unrelated notifications in the new guest.
        clear(notifyGuest: false)
        session = UUID(); limiter = initialLimiter; stopped = false
        bindResponses()
    }
    public func stop() {
        stopped = true
        clear(notifyGuest: true)
        center.onResponse = nil
    }
    private func clear(notifyGuest: Bool) {
        for id in Array(active.keys) {
            let revision = active[id]!.revision
            remove(id: id)
            if notifyGuest { send(.notificationClosed(id: id, revision: revision, reason: .undefined)) }
        }
    }

    @discardableResult func handleResponse(identifier: String, action: String?) -> Bool {
        guard !stopped, let (id, entry) = active.first(where: { $0.value.identifier == identifier }),
              action == nil || entry.actions.contains(action!) else { return false }
        guard isEnabled() else { reject(entry, id: id); return true }
        remove(id: id)
        if let action { send(.notificationAction(id: id, revision: entry.revision, key: action)) }
        send(.notificationClosed(id: id, revision: entry.revision, reason: .dismissed))
        return true
    }

    deinit {
        let center = center, identifiers = active.values.map(\.identifier)
        let archive = archive
        let cancelled = active.values.compactMap { entry -> GuestNotificationHistoryRecord? in
            guard entry.record.delivery == .pending else { return nil }
            var record = entry.record
            record.delivery = .cancelled
            return record
        }
        active.values.forEach { $0.task?.cancel() }
        Task { @MainActor in
            cancelled.forEach { archive?($0) }
            center.onResponse = nil
            identifiers.forEach { center.withdraw(identifier: $0) }
        }
    }
}

@MainActor
private final class UnavailableGuestNotificationCenter: GuestNotificationCenter {
    var onResponse: ((String, String?) -> Bool)?
    func authorize() async -> Bool { false }
    func present(identifier: String, content: GuestNotificationContent) async throws {}
    func withdraw(identifier: String) {}
}

/// A click on a notification presented by another process reaches the display
/// that posted it only if that process is named here.
public enum GuestNotificationForwarding {
    @MainActor public static func trust(bundle identifier: String) {
        GuestNotificationResponseRouter.trustForwarder(bundle: identifier)
    }
}

@MainActor
final class SystemGuestNotificationCenter: GuestNotificationCenter {
    private let owner = UUID()
    private let responseDirectory: URL
    private let backend: GuestNotificationBackend
    private var router: GuestNotificationResponseRouter?
    init(responseDirectory: URL, backend: GuestNotificationBackend? = nil) {
        self.responseDirectory = responseDirectory
        self.backend = backend ?? LocalGuestNotificationBackend()
    }
    var onResponse: ((String, String?) -> Bool)? {
        didSet {
            router?.stop(); router = nil
            backend.attach(owner: owner, directory: nil, responder: nil)
            guard let onResponse else { return }
            do {
                let route = try GuestNotificationResponseRouter(directory: responseDirectory, receive: onResponse)
                router = route
                backend.attach(owner: owner, directory: route.directory, responder: onResponse)
            } catch { /* No reliable action route: decline posts rather than lose guest actions. */ }
        }
    }
    func authorize() async -> Bool {
        guard router != nil else { return false }
        return await backend.authorization(prompt: true).allowsDelivery
    }
    func present(identifier: String, content: GuestNotificationContent) async throws {
        guard let router else { throw CancellationError() }
        try await backend.present(identifier: identifier, content: content, responsePath: router.url.path)
    }
    func withdraw(identifier: String) { backend.withdraw(identifier: identifier) }

    static func categoryIdentifier(_ actions: [GuestNotificationContent.Action]) -> String {
        GuestNotificationCategories.identifier(actions)
    }
    static func mergeCategories(_ existing: Set<UNNotificationCategory>, used: Set<String>,
                                actions: [GuestNotificationContent.Action]) -> Set<UNNotificationCategory> {
        GuestNotificationCategories.merge(existing, used: used, actions: actions)
    }

}
