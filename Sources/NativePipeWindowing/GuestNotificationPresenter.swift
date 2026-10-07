import Foundation
import CryptoKit
import Darwin
import NativePipeProtocol
import UserNotifications

/// What macOS is asked to show for one guest notification.
struct GuestNotificationContent: Equatable {
    struct Action: Codable, Hashable {
        var key: String
        var label: String
    }
    var title: String
    var subtitle: String
    var body: String
    var actions: [Action]
    /// Seconds after which the host withdraws it. Nil leaves it to macOS.
    var expiry: TimeInterval?
    /// Low urgency is delivered without interrupting; others as normal alerts.
    var quiet: Bool
}

/// Everything a guest sends is untrusted text: any program in the machine can
/// post notifications. These rules bound size and strip anything that is not
/// plain text before macOS sees it.
enum GuestNotificationPolicy {
    static let maximumTitleLength = 120
    static let maximumBodyLength = 600
    static let maximumAppNameLength = 40
    static let maximumActionLabelLength = 40
    static let maximumActionKeyLength = 128
    /// macOS shows only a few buttons; the click on the banner is "default".
    static let maximumActions = 4
    static let maximumExpiry: TimeInterval = 3600
    static let defaultActionKey = "default"

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
        var task: Task<Void, Never>?
        init(identifier: String, revision: UInt64, actions: Set<String>) {
            self.identifier = identifier; self.revision = revision; self.actions = actions
        }
    }
    private let machine: String
    private let machineIdentity: String
    private let center: GuestNotificationCenter
    private let isEnabled: () -> Bool
    private let now: () -> TimeInterval
    private let send: (Windowing.HostCommand) -> Void
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
                            isEnabled: @escaping () -> Bool = { true },
                            send: @escaping (Windowing.HostCommand) -> Void) {
        let center: GuestNotificationCenter = Bundle.main.bundleURL.pathExtension == "app"
            && Bundle.main.bundleIdentifier != nil
            ? SystemGuestNotificationCenter(responseDirectory: responseDirectory) : UnavailableGuestNotificationCenter()
        self.init(machine: machine, identity: identity, center: center, isEnabled: isEnabled, send: send)
    }

    init(machine: String, identity: String = "test-machine", center: GuestNotificationCenter,
         isEnabled: @escaping () -> Bool = { true }, limiter: GuestNotificationRateLimiter = .init(),
         now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
         send: @escaping (Windowing.HostCommand) -> Void) {
        self.machine = machine
        machineIdentity = SHA256.hash(data: Data(identity.utf8)).map { String(format: "%02x", $0) }.joined()
        self.center = center; self.isEnabled = isEnabled; self.now = now; self.send = send
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
        let entry = Entry(identifier: identifier, revision: notification.revision,
            actions: Set(content.actions.map(\.key) + [GuestNotificationPolicy.defaultActionKey]))
        active[id] = entry
        entry.task = Task { @MainActor [weak self, entry, center] in
            defer { entry.task = nil }
            let allowed = await center.authorize() // Fresh settings; denied is not cached forever.
            guard self?.owns(entry, id: id) == true else { return }
            guard !Task.isCancelled, self?.isEnabled() == true else { self?.reject(entry, id: id); return }
            guard allowed else { self?.reject(entry, id: id); return }
            do { try await center.present(identifier: identifier, content: content) }
            catch {
                // This task may belong to an earlier replacement. Its failure
                // must never remove the current notification's record.
                if self?.owns(entry, id: id) == true { self?.reject(entry, id: id) }
                else { center.withdraw(identifier: identifier) }
                return
            }
            guard self?.owns(entry, id: id) == true, !Task.isCancelled, self?.isEnabled() == true else {
                center.withdraw(identifier: identifier)
                if self?.owns(entry, id: id) == true { self?.reject(entry, id: id) }
                return
            }
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
        active.values.forEach { $0.task?.cancel() }
        Task { @MainActor in
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

/// One native delegate per process; creating another VM/remote owner must not
/// replace the first owner's response handler.
@MainActor
private final class GuestNotificationSystem: NSObject, UNUserNotificationCenterDelegate {
    static let shared = GuestNotificationSystem()
    let center = UNUserNotificationCenter.current()
    nonisolated static let responseSocketKey = "nativepipe.response-socket"
    var responses: [UUID: (String, String?) -> Bool] = [:]
    // A stopped local owner must still forward another live VM's response.
    // These are configured host directories, never paths supplied by guests.
    var responseDirectories: Set<URL> = []
    private var authorization: Task<Bool, Never>?

    override init() { super.init(); center.delegate = self }
    func authorize() async -> Bool {
        let settings = await center.notificationSettings()
        switch settings.authorizationStatus {
        case .authorized, .provisional, .ephemeral: return true
        case .denied: return false
        case .notDetermined:
            if let authorization { return await authorization.value }
            let task = Task { (try? await center.requestAuthorization(options: [.alert])) ?? false }
            authorization = task
            let result = await task.value
            authorization = nil
            return result
        @unknown default: return false
        }
    }
    nonisolated func userNotificationCenter(
        _ center: UNUserNotificationCenter, willPresent notification: UNNotification
    ) async -> UNNotificationPresentationOptions { [.banner, .list] }
    nonisolated func userNotificationCenter(
        _ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse
    ) async {
        let identifier = response.notification.request.identifier
        let action: String?
        switch response.actionIdentifier {
        case UNNotificationDismissActionIdentifier: action = nil
        case UNNotificationDefaultActionIdentifier: action = GuestNotificationPolicy.defaultActionKey
        default: action = response.actionIdentifier
        }
        let path = response.notification.request.content.userInfo[Self.responseSocketKey] as? String
        await deliver(identifier: identifier, action: action, path: path)
    }
    private func deliver(identifier: String, action: String?, path: String?) async {
        if responses.values.contains(where: { $0(identifier, action) }) { return }
        guard let path else { return }
        _ = await GuestNotificationResponseRouter.forward(.init(identifier: identifier, action: action),
            path: path, permittedDirectories: responseDirectories)
    }
}

@MainActor
final class SystemGuestNotificationCenter: GuestNotificationCenter {
    private let owner = UUID()
    private let responseDirectory: URL
    private var router: GuestNotificationResponseRouter?
    init(responseDirectory: URL) { self.responseDirectory = responseDirectory }
    var onResponse: ((String, String?) -> Bool)? {
        didSet {
            router?.stop(); router = nil
            GuestNotificationSystem.shared.responses[owner] = nil
            guard let onResponse else { return }
            do {
                let route = try GuestNotificationResponseRouter(directory: responseDirectory, receive: onResponse)
                router = route
                GuestNotificationSystem.shared.responseDirectories.insert(route.directory)
                GuestNotificationSystem.shared.responses[owner] = onResponse
            } catch { /* No reliable action route: decline posts rather than lose guest actions. */ }
        }
    }
    func authorize() async -> Bool {
        guard router != nil else { return false }
        return await GuestNotificationSystem.shared.authorize()
    }

    static func categoryIdentifier(_ actions: [GuestNotificationContent.Action]) -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        let data = (try? encoder.encode(actions)) ?? Data()
        return "nativepipe.guest.actions." + SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    static func mergeCategories(_ existing: Set<UNNotificationCategory>, used: Set<String>,
                                actions: [GuestNotificationContent.Action]) -> Set<UNNotificationCategory> {
        var result = Set(existing.filter {
            !$0.identifier.hasPrefix("nativepipe.guest.actions.") || used.contains($0.identifier)
        })
        result.insert(UNNotificationCategory(identifier: categoryIdentifier(actions),
            actions: actions.map { UNNotificationAction(identifier: $0.key, title: $0.label, options: []) },
            intentIdentifiers: [], options: [.customDismissAction]))
        return result
    }

    func present(identifier: String, content: GuestNotificationContent) async throws {
        guard let router else { throw CancellationError() }
        let center = GuestNotificationSystem.shared.center
        // Categories are application-wide, including other VMHost processes.
        // Hold a cooperative private lock across read/merge/add, using async
        // backoff so a competing VM never blocks its main actor.
        let path = FileManager.default.temporaryDirectory.appendingPathComponent("nativepipe-notification-categories.lock").path
        let descriptor = Darwin.open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0o600)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { flock(descriptor, LOCK_UN); Darwin.close(descriptor) }
        while flock(descriptor, LOCK_EX | LOCK_NB) != 0 {
            guard errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            try Task.checkCancellation()
            try await Task.sleep(for: .milliseconds(25))
        }
        try Task.checkCancellation()
        let existing = await center.notificationCategories()
        let delivered = await center.deliveredNotifications()
        let pending = await center.pendingNotificationRequests()
        try Task.checkCancellation()
        let used = Set(delivered.map { $0.request.content.categoryIdentifier } + pending.map { $0.content.categoryIdentifier })
        let category = Self.categoryIdentifier(content.actions)
        center.setNotificationCategories(Self.mergeCategories(existing, used: used, actions: content.actions))
        let value = UNMutableNotificationContent()
        value.title = content.title; value.subtitle = content.subtitle; value.body = content.body
        value.threadIdentifier = identifier.components(separatedBy: ".").prefix(4).joined(separator: ".")
        if content.quiet { value.interruptionLevel = .passive }
        value.categoryIdentifier = category
        value.userInfo[GuestNotificationSystem.responseSocketKey] = router.url.path
        try await center.add(UNNotificationRequest(identifier: identifier, content: value, trigger: nil))
    }
    func withdraw(identifier: String) {
        let center = GuestNotificationSystem.shared.center
        center.removeDeliveredNotifications(withIdentifiers: [identifier])
        center.removePendingNotificationRequests(withIdentifiers: [identifier])
    }
}
