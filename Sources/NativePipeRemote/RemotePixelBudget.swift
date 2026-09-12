import Foundation

/// Reservations follow pixel owners, including owners retained by Metal after
/// cache eviction or reconnect. Decoder reference memory is conservatively
/// reserved separately; these counters are admission costs, not measured RSS.
final class RemotePixelBudget: @unchecked Sendable {
    enum Kind { case decoder, frame, alpha, replay }
    struct Snapshot: Sendable {
        let limit, decoder, frame, alpha, replay: Int
        var total: Int { decoder + frame + alpha + replay }
    }
    let limit: Int
    private let lock = NSLock()
    private var costs: [Kind: Int] = [:]
    init(limit: Int) { self.limit = max(0, limit) }

    var snapshot: Snapshot {
        lock.lock(); defer { lock.unlock() }
        return .init(limit: limit, decoder: costs[.decoder, default: 0],
                     frame: costs[.frame, default: 0], alpha: costs[.alpha, default: 0],
                     replay: costs[.replay, default: 0])
    }

    func reserve(_ bytes: Int, kind: Kind) -> Lease? {
        guard resize(kind: kind, from: 0, to: bytes) else { return nil }
        return Lease(budget: self, bytes: bytes, kind: kind)
    }

    private func resize(kind: Kind, from previous: Int, to next: Int) -> Bool {
        lock.lock(); defer { lock.unlock() }
        let total = costs.values.reduce(0, +)
        guard next >= 0, next <= limit - total + previous else { return false }
        costs[kind, default: 0] += next - previous
        return true
    }

    final class Lease {
        private let budget: RemotePixelBudget
        private let kind: Kind
        private(set) var bytes: Int
        fileprivate init(budget: RemotePixelBudget, bytes: Int, kind: Kind) {
            self.budget = budget; self.bytes = bytes; self.kind = kind
        }
        // Only decodeQueue resizes a lease, before it is handed to consumers.
        func resize(to bytes: Int) -> Bool {
            guard budget.resize(kind: kind, from: self.bytes, to: bytes) else { return false }
            self.bytes = bytes
            return true
        }
        deinit { _ = budget.resize(kind: kind, from: bytes, to: 0) }
    }
}
