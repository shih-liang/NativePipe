import CryptoKit
import Darwin
import Foundation
import NativePipeProtocol
import UserNotifications

/// Notification categories are application-wide. Each distinct set of guest
/// actions is one category, and unused ones are pruned when another is added.
public enum GuestNotificationCategories {
    public static func identifier(_ actions: [GuestNotificationContent.Action]) -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        let data = (try? encoder.encode(actions)) ?? Data()
        return "nativepipe.guest.actions." + SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    public static func merge(_ existing: Set<UNNotificationCategory>, used: Set<String>,
                      actions: [GuestNotificationContent.Action]) -> Set<UNNotificationCategory> {
        var result = Set(existing.filter {
            !$0.identifier.hasPrefix("nativepipe.guest.actions.") || used.contains($0.identifier)
        })
        result.insert(UNNotificationCategory(identifier: identifier(actions),
            actions: actions.map { UNNotificationAction(identifier: $0.key, title: $0.label, options: []) },
            intentIdentifiers: [], options: [.customDismissAction]))
        return result
    }
}

/// One native delegate per process; creating another VM/remote owner must not
/// replace the first owner's response handler.
@MainActor
final class GuestNotificationSystem: NSObject, UNUserNotificationCenterDelegate {
    static let shared = GuestNotificationSystem()
    let center = UNUserNotificationCenter.current()
    nonisolated static let responseSocketKey = "nativepipe.response-socket"
    var responses: [UUID: (String, String?) -> Bool] = [:]
    // A stopped local owner must still forward another live VM's response.
    // These are configured host directories, never paths supplied by guests.
    var responseDirectories: Set<URL> = []
    private var authorization: Task<Bool, Never>?

    override init() { super.init(); center.delegate = self }
    func authorization(prompt: Bool) async -> GuestNotificationAuthorization {
        func state(_ settings: UNNotificationSettings) -> GuestNotificationAuthorization {
            switch settings.authorizationStatus {
            case .authorized, .provisional, .ephemeral:
                // "None" as the alert style, or alerts switched off, still
                // authorizes the app: everything goes silently to the center.
                return settings.alertStyle == .none || settings.alertSetting == .disabled ? .alertsOff : .authorized
            case .denied: return .denied
            case .notDetermined: return .notDetermined
            @unknown default: return .denied
            }
        }
        let current = state(await center.notificationSettings())
        guard current == .notDetermined, prompt else { return current }
        // Concurrent callers share one system prompt; a denial is read fresh
        // next time rather than cached.
        if let authorization { _ = await authorization.value }
        else {
            let task = Task { (try? await center.requestAuthorization(options: [.alert])) ?? false }
            authorization = task
            _ = await task.value
            authorization = nil
        }
        return state(await center.notificationSettings())
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
        case UNNotificationDefaultActionIdentifier: action = GuestNotificationLimits.defaultActionKey
        default: action = response.actionIdentifier
        }
        let path = response.notification.request.content.userInfo[Self.responseSocketKey] as? String
        await deliver(identifier: identifier, action: action, path: path)
    }
    private func deliver(identifier: String, action: String?, path: String?) async {
        if responses.values.contains(where: { $0(identifier, action) }) { return }
        guard let path else { return }
        _ = await GuestNotificationResponseForwarder.forward(.init(identifier: identifier, action: action),
            path: path, permittedDirectories: responseDirectories)
    }
}

/// Presents notifications from this process. Used by the resident service, and
/// by display processes that have no service to hand them to.
@MainActor
public final class LocalGuestNotificationBackend: GuestNotificationBackend {
    public init() {}
    public func authorization(prompt: Bool) async -> GuestNotificationAuthorization {
        await GuestNotificationSystem.shared.authorization(prompt: prompt)
    }
    public func attach(owner: UUID, directory: URL?, responder: ((String, String?) -> Bool)?) {
        let system = GuestNotificationSystem.shared
        system.responses[owner] = responder
        if let directory { system.responseDirectories.insert(directory) }
    }
    /// Clicks on notifications posted for other processes are forwarded to the
    /// response sockets under this directory, and nowhere else.
    public func permitResponses(inIPCDirectory directory: URL) {
        GuestNotificationSystem.shared.responseDirectories.insert(
            GuestNotificationResponseForwarder.endpointDirectory(in: directory))
    }
    public func present(identifier: String, content: GuestNotificationContent, responsePath: String) async throws {
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
        let category = GuestNotificationCategories.identifier(content.actions)
        center.setNotificationCategories(GuestNotificationCategories.merge(existing, used: used, actions: content.actions))
        let value = UNMutableNotificationContent()
        value.title = content.title; value.subtitle = content.subtitle; value.body = content.body
        value.threadIdentifier = identifier.components(separatedBy: ".").prefix(4).joined(separator: ".")
        if content.quiet { value.interruptionLevel = .passive }
        value.categoryIdentifier = category
        value.userInfo[GuestNotificationSystem.responseSocketKey] = responsePath
        try await center.add(UNNotificationRequest(identifier: identifier, content: value, trigger: nil))
    }
    public func withdraw(identifier: String) {
        let center = GuestNotificationSystem.shared.center
        center.removeDeliveredNotifications(withIdentifiers: [identifier])
        center.removePendingNotificationRequests(withIdentifiers: [identifier])
    }
}
