import Foundation

public enum GuestNotificationDelivery: String, Codable, Sendable {
    case pending
    case delivered
    case notAuthorized
    case failed
    case cancelled
}

/// What macOS currently lets the notification owner do. `alertsOff` is
/// authorized, but the alert style is "None" so nothing appears on screen.
public enum GuestNotificationAuthorization: String, Codable, Sendable, Equatable, CaseIterable {
    case notDetermined
    case authorized
    case denied
    case alertsOff

    /// Notifications may be handed to macOS: the center keeps them even when
    /// the user turned banners off.
    public var allowsDelivery: Bool { self == .authorized || self == .alertsOff }
}

/// The bounded, plain text content accepted by the host, identified by its
/// system notification ID rather than a reusable guest ID.
public struct GuestNotificationHistoryRecord: Codable, Identifiable, Sendable, Equatable {
    public var id: String
    public var machine: String
    public var application: String
    public var title: String
    public var body: String
    public var receivedAt: Date
    public var delivery: GuestNotificationDelivery

    public init(id: String, machine: String, application: String, title: String, body: String,
                receivedAt: Date, delivery: GuestNotificationDelivery) {
        self.id = id
        self.machine = machine
        self.application = application
        self.title = title
        self.body = body
        self.receivedAt = receivedAt
        self.delivery = delivery
    }
}
