import Foundation

/// Output facts are updated by AppKit, then frozen at the Metal commit boundary.
/// The drawable callback never consults a moved or already destroyed NSWindow.
final class PresentationOutputState: @unchecked Sendable {
    struct Snapshot: Sendable, Equatable {
        var outputID: UInt32 = 0
        var refreshNanoseconds: UInt32 = 0
    }
    private let lock = NSLock()
    private var value = Snapshot()

    func update(outputID: UInt32, refreshNanoseconds: UInt32) {
        lock.lock()
        value = Snapshot(outputID: outputID, refreshNanoseconds: refreshNanoseconds)
        lock.unlock()
    }

    func snapshot() -> Snapshot {
        lock.lock()
        defer { lock.unlock() }
        return value
    }
}

/// Actual display results survive UI visibility and transport generations.
/// Only the guest's receipt retires a result; this journal owns no pixel objects.
final class PresentationJournal: @unchecked Sendable {
    struct Key: Hashable, Sendable {
        let sessionID: UInt64
        let clockEpoch: UInt64
        let surface: UInt32
        let presentationID: UInt32
    }

    struct Result: Equatable, Sendable {
        let key: Key
        let hostTimeNanoseconds: UInt64
        let refreshNanoseconds: UInt32
        let outputID: UInt32
    }

    private let lock = NSLock()
    private var records: [Key: PresentationRecord] = [:]
    private var nextSequence: UInt64 = 0
    private let changed: @Sendable () -> Void

    init(changed: @escaping @Sendable () -> Void = {}) { self.changed = changed }

    func register(_ key: Key) -> PresentationRecord {
        lock.lock()
        defer { lock.unlock() }
        if let record = records[key] { return record }
        nextSequence &+= 1
        let record = PresentationRecord(key: key, sequence: nextSequence, journal: self)
        records[key] = record
        return record
    }

    /// A new nonce proves the previous compositor no longer owns any feedback.
    /// A transport detach alone must never call this with a different nonce.
    func retainSession(_ sessionID: UInt64) {
        lock.lock()
        records = records.filter { $0.key.sessionID == sessionID }
        lock.unlock()
    }

    func results(sessionID: UInt64) -> [Result] {
        snapshot().filter { $0.key.sessionID == sessionID }
            .sorted { $0.sequence < $1.sequence }.compactMap { $0.result }
    }

    func hasSubmitted(sessionID: UInt64? = nil) -> Bool {
        snapshot().contains {
            (sessionID == nil || $0.key.sessionID == sessionID) && $0.isSubmitted
        }
    }

    func discardUnsubmitted(sessionID: UInt64? = nil) {
        for record in snapshot() where sessionID == nil || record.key.sessionID == sessionID {
            record.discardIfUnsubmitted()
        }
    }

    func acknowledge(_ key: Key) {
        lock.lock()
        let record = records[key]
        lock.unlock()
        guard let record, record.result != nil else { return }
        lock.lock()
        if records[key] === record { records[key] = nil }
        lock.unlock()
    }

    private func snapshot() -> [PresentationRecord] {
        lock.lock()
        defer { lock.unlock() }
        return Array(records.values)
    }

    fileprivate func notifyChanged() { changed() }
}

/// A queued scene can be proven discarded. Once committed, only the drawable's
/// actual callback can establish its result, even if its GPU reads fail later.
final class PresentationRecord: @unchecked Sendable {
    let key: PresentationJournal.Key
    fileprivate let sequence: UInt64
    private weak var journal: PresentationJournal?
    private let lock = NSLock()
    private enum State {
        case queued
        case submitted(PresentationOutputState.Snapshot)
        case terminal(PresentationJournal.Result)
    }
    private var state = State.queued
    private var output: PresentationOutputState?

    fileprivate init(key: PresentationJournal.Key, sequence: UInt64, journal: PresentationJournal) {
        self.key = key
        self.sequence = sequence
        self.journal = journal
    }

    func useOutput(_ output: PresentationOutputState) {
        lock.lock()
        if case .queued = state { self.output = output }
        lock.unlock()
    }

    /// Called under the presenter's epoch/gate lock immediately before commit.
    func claimSubmission() -> Bool {
        lock.lock()
        guard case .queued = state else { lock.unlock(); return false }
        state = .submitted(output?.snapshot() ?? .init())
        output = nil
        lock.unlock()
        journal?.notifyChanged()
        return true
    }

    @discardableResult
    func discardIfUnsubmitted() -> Bool {
        lock.lock()
        guard case .queued = state else { lock.unlock(); return false }
        state = .terminal(.init(key: key, hostTimeNanoseconds: 0,
                               refreshNanoseconds: 0, outputID: 0))
        output = nil
        lock.unlock()
        journal?.notifyChanged()
        return true
    }

    /// Runs directly on Metal's callback thread, before any main-actor work.
    func recordDrawableResult(presentedTime: Double) {
        // Metal defines zero as a skipped drawable. Invalid numeric data is
        // unknown, not evidence that a committed frame never appeared.
        guard presentedTime.isFinite, presentedTime >= 0 else { return }
        let scaled = (presentedTime * 1_000_000_000).rounded()
        guard scaled.isFinite, scaled < Double(UInt64.max),
              presentedTime == 0 || scaled >= 1 else { return }
        lock.lock()
        guard case .submitted(let metadata) = state else { lock.unlock(); return }
        let nanoseconds = UInt64(scaled)
        state = .terminal(.init(key: key, hostTimeNanoseconds: nanoseconds,
                               refreshNanoseconds: metadata.refreshNanoseconds,
                               outputID: metadata.outputID))
        lock.unlock()
        journal?.notifyChanged()
    }

    fileprivate var result: PresentationJournal.Result? {
        lock.lock()
        defer { lock.unlock() }
        if case .terminal(let result) = state { return result }
        return nil
    }

    /// Only a known zero result authorizes a fresh display attempt. An invalid
    /// callback or still-submitted drawable remains unknown and must not replay.
    var hasDiscardResult: Bool {
        lock.lock()
        defer { lock.unlock() }
        if case .terminal(let result) = state { return result.hostTimeNanoseconds == 0 }
        return false
    }

    fileprivate var isSubmitted: Bool {
        lock.lock()
        defer { lock.unlock() }
        if case .submitted = state { return true }
        return false
    }
}
