import Foundation

public enum GuestNotificationDelivery: String, Codable, Sendable {
    case pending
    case delivered
    case notAuthorized
    case failed
    case cancelled
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
