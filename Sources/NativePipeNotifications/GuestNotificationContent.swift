import Foundation

/// What macOS is asked to show for one guest notification.
public struct GuestNotificationContent: Codable, Equatable, Sendable {
    public struct Action: Codable, Hashable, Sendable {
        public var key: String
        public var label: String
        public init(key: String, label: String) { self.key = key; self.label = label }
    }
    public var title: String
    public var subtitle: String
    public var body: String
    public var actions: [Action]
    /// Seconds after which the host withdraws it. Nil leaves it to macOS.
    public var expiry: TimeInterval?
    /// Low urgency is delivered without interrupting; others as normal alerts.
    public var quiet: Bool
    public init(title: String, subtitle: String, body: String, actions: [Action],
                expiry: TimeInterval?, quiet: Bool) {
        self.title = title; self.subtitle = subtitle; self.body = body
        self.actions = actions; self.expiry = expiry; self.quiet = quiet
    }
}


/// Bounds on what is accepted from a guest or from a display process. The
/// guest policy sanitizes against them; the presenting process re-checks.
public enum GuestNotificationLimits {
    public static let title = 120
    public static let body = 600
    public static let appName = 40
    public static let actionLabel = 40
    public static let actionKey = 128
    public static let actions = 4
    /// Seconds after which the host withdraws a notification at the latest.
    public static let expiry: TimeInterval = 3600
    public static let subtitle = title + appName + 3
    public static let identifierBytes = 256
    /// The click on the banner itself, as opposed to one of the guest's actions.
    public static let defaultActionKey = "default"
    public static let identifierPrefix = "nativepipe.guest."
}

public extension GuestNotificationContent {
    /// A content accepted from another process must already fit the limits.
    var isWithinLimits: Bool {
        title.count <= GuestNotificationLimits.title && body.count <= GuestNotificationLimits.body
            && subtitle.count <= GuestNotificationLimits.subtitle
            && actions.count <= GuestNotificationLimits.actions
            && actions.allSatisfy { !$0.key.isEmpty && $0.key.utf8.count <= GuestNotificationLimits.actionKey
                && !$0.label.isEmpty && $0.label.count <= GuestNotificationLimits.actionLabel }
            && (expiry.map { $0 >= 0 && $0 <= GuestNotificationLimits.expiry } ?? true)
    }
}
