import Foundation

/// Transport display credit is independent of protected source-buffer reads.
/// A late Core Animation callback must not return a retired credit twice.
@MainActor
final class ScenePresentationCompletion {
    private var callback: ((Bool, Double?) -> Void)?
    init(_ callback: @escaping (Bool, Double?) -> Void) { self.callback = callback }
    func finish(_ displayed: Bool, presentedTime: Double? = nil) {
        let callback = self.callback
        self.callback = nil
        callback?(displayed, presentedTime)
    }
}

@MainActor
final class ScenePresentationCredits {
    private var outstanding: [UInt32: ScenePresentationCompletion] = [:]

    func register(
        _ presentationID: UInt32, callback: @escaping (Bool) -> Void
    ) -> ScenePresentationCompletion {
        registerTimed(presentationID) { displayed, _ in callback(displayed) }
    }

    func registerTimed(
        _ presentationID: UInt32, callback: @escaping (Bool, Double?) -> Void
    ) -> ScenePresentationCompletion {
        outstanding.removeValue(forKey: presentationID)?.finish(false)
        let completion = ScenePresentationCompletion { [weak self] displayed, time in
            self?.outstanding.removeValue(forKey: presentationID)
            callback(displayed, time)
        }
        outstanding[presentationID] = completion
        return completion
    }

    func discardUnpresented() {
        let pending = Array(outstanding.values)
        outstanding.removeAll()
        for completion in pending { completion.finish(false) }
    }
}

/// Occluded discard credits stay bounded by the guest's window flight limit.
/// Explicit capture or restored visibility can release that deferred batch.
struct DeferredSceneFeedback {
    private var deferred: [UInt32: [UInt32]] = [:]

    mutating func shouldSend(
        surface: UInt32, presentationID: UInt32, displayed: Bool, occluded: Bool
    ) -> Bool {
        if !displayed, occluded {
            deferred[surface, default: []].append(presentationID)
            return false
        }
        return true
    }

    mutating func take(surface: UInt32) -> [UInt32] {
        deferred.removeValue(forKey: surface) ?? []
    }

    mutating func reset() { deferred.removeAll() }
}
