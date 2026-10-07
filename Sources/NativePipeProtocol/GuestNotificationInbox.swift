import Foundation

/// Optional notification state for one transport generation. The caller owns
/// synchronization. Unlike scene commits, replacements have no pixel lease and
/// can be collapsed before delivery to the main run loop.
public struct GuestNotificationInbox: Sendable {
    public static let capacity = 64 // The compositor's active-notification limit.
    /// Three independent bounded maps plus one coalesced reset flag.
    public static let maximumPendingCount = 3 * capacity + 1
    private var posted: [UInt32: Windowing.GuestNotification] = [:]
    private var closed: [UInt32: UInt64] = [:]
    private var rejected: [UInt32: UInt64] = [:]
    private var reset = false

    public init() {}
    public var isEmpty: Bool { posted.isEmpty && closed.isEmpty && rejected.isEmpty && !reset }
    public var pendingCount: Int { posted.count + closed.count + rejected.count + (reset ? 1 : 0) }

    /// Returns false for structural events, which must retain their own FIFO.
    @discardableResult
    public mutating func offer(_ event: Windowing.GuestEvent) -> Bool {
        switch event {
        case .notificationPosted(let note):
            guard note.id != 0, note.revision != 0 else { return true }
            guard note.revision > (closed[note.id] ?? 0),
                  note.revision > (rejected[note.id] ?? 0),
                  note.revision >= (posted[note.id]?.revision ?? 0) else { return true }
            closed[note.id] = nil
            rejected[note.id] = nil
            if posted[note.id] != nil || posted.count < Self.capacity {
                posted[note.id] = note
            } else {
                // A conforming guest has at most 64 active IDs. Excess unknown
                // IDs are optional input; do not grow the queue or kill graphics.
                reset = true
                reject(id: note.id, revision: note.revision)
            }
        case .notificationClosed(let id, let revision):
            guard id != 0, revision != 0 else { return true }
            guard revision >= (posted[id]?.revision ?? 0),
                  revision >= (rejected[id] ?? 0) else { return true }
            posted[id] = nil
            rejected[id] = nil
            if closed[id] != nil || closed.count < Self.capacity {
                closed[id] = max(closed[id] ?? 0, revision)
            } else {
                // Unknown historical closes can churn indefinitely even with
                // only 64 live IDs. One local reset removes any lost close's
                // banner, with at most 64 replies at the next UI drain.
                reset = true
                closed.removeAll(keepingCapacity: true)
                closed[id] = revision
            }
        case .notificationRejected(let id, let revision):
            guard let id, let revision, id != 0, revision != 0 else { return true }
            reject(id: id, revision: revision)
        case .notificationBacklogReset:
            reset = true
        default:
            return false
        }
        return true
    }

    private mutating func reject(id: UInt32, revision: UInt64) {
        guard revision >= (posted[id]?.revision ?? 0),
              revision > (closed[id] ?? 0) else { return }
        posted[id] = nil
        closed[id] = nil
        if rejected[id] != nil || rejected.count < Self.capacity {
            rejected[id] = max(rejected[id] ?? 0, revision)
        } else {
            // The writer observes guest retirements before this inbox. Thus
            // conforming live IDs fit; excess unknown IDs must not create an
            // unbounded rejection list or an outgoing acknowledgement flood.
            reset = true
        }
    }

    public mutating func drain() -> [Windowing.GuestEvent] {
        var result: [Windowing.GuestEvent] = []
        result.reserveCapacity(pendingCount)
        if reset { result.append(.notificationBacklogReset) }
        for id in closed.keys.sorted() {
            result.append(.notificationClosed(id: id, revision: closed[id]!))
        }
        for id in rejected.keys.sorted() {
            result.append(.notificationRejected(id: id, revision: rejected[id]!))
        }
        for id in posted.keys.sorted() { result.append(.notificationPosted(posted[id]!)) }
        clear()
        return result
    }

    public mutating func clear() {
        posted.removeAll(keepingCapacity: true)
        closed.removeAll(keepingCapacity: true)
        rejected.removeAll(keepingCapacity: true)
        reset = false
    }
}
