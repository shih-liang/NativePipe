import Foundation
import XCTest
@testable import NativePipeWindowing

final class PresentationJournalTests: XCTestCase {
    private func key(_ id: UInt32, session: UInt64 = 7, epoch: UInt64 = 1) -> PresentationJournal.Key {
        .init(sessionID: session, clockEpoch: epoch, surface: 2, presentationID: id)
    }

    func testSubmittedResultSurvivesVisibilityCaptureAndReadFailureRetirement() {
        let journal = PresentationJournal()
        let record = journal.register(key(1))
        let output = PresentationOutputState()
        output.update(outputID: 9, refreshNanoseconds: 16_666_667)
        record.useOutput(output)
        XCTAssertTrue(record.claimSubmission())
        XCTAssertFalse(record.discardIfUnsubmitted())
        journal.discardUnsubmitted()
        XCTAssertTrue(journal.hasSubmitted())
        XCTAssertTrue(journal.results(sessionID: 7).isEmpty)

        // A later screen change cannot rewrite the output used at submission.
        output.update(outputID: 10, refreshNanoseconds: 8_333_333)
        DispatchQueue.global().sync { record.recordDrawableResult(presentedTime: 102.125) }
        XCTAssertFalse(journal.hasSubmitted())
        XCTAssertEqual(journal.results(sessionID: 7), [
            .init(key: key(1), hostTimeNanoseconds: 102_125_000_000,
                  refreshNanoseconds: 16_666_667, outputID: 9)
        ])
        record.recordDrawableResult(presentedTime: 103)
        XCTAssertFalse(record.discardIfUnsubmitted())
        XCTAssertEqual(journal.results(sessionID: 7).first?.hostTimeNanoseconds, 102_125_000_000)
    }

    @MainActor func testRemoteCreditRetirementCannotConsumeTheActualDrawableTimestamp() {
        let journal = PresentationJournal()
        let record = journal.register(key(1))
        let credits = ScenePresentationCredits()
        var remoteCredits: [Bool] = []
        let credit = credits.register(1) { remoteCredits.append($0) }
        XCTAssertTrue(record.claimSubmission())
        credits.discardUnpresented()
        XCTAssertEqual(remoteCredits, [false])
        // Store the fact while the main actor is occupied by this test. It must
        // be available before any queued UI callback can run.
        DispatchQueue.global().sync { record.recordDrawableResult(presentedTime: 200.5) }
        XCTAssertEqual(journal.results(sessionID: 7).first?.hostTimeNanoseconds, 200_500_000_000)
        credit.finish(true)
        XCTAssertEqual(remoteCredits, [false])
    }

    func testOnlyAnAcknowledgedTerminalResultIsRetiredAcrossReconnectAndClockEpochs() {
        let journal = PresentationJournal()
        let old = journal.register(key(1))
        XCTAssertTrue(old.claimSubmission())
        journal.acknowledge(key(1))
        XCTAssertTrue(journal.hasSubmitted(), "An early receipt cannot erase an unknown display result")
        journal.retainSession(7)
        old.recordDrawableResult(presentedTime: 12)
        let current = journal.register(key(1, epoch: 2))
        XCTAssertTrue(current.discardIfUnsubmitted())
        XCTAssertEqual(journal.results(sessionID: 7).map(\.key.clockEpoch), [1, 2])
        journal.acknowledge(key(1, epoch: 99))
        XCTAssertEqual(journal.results(sessionID: 7).count, 2)
        journal.acknowledge(key(1))
        journal.acknowledge(key(1))
        XCTAssertEqual(journal.results(sessionID: 7).map(\.key.clockEpoch), [2])
        old.recordDrawableResult(presentedTime: 13)
        XCTAssertEqual(journal.results(sessionID: 7).count, 1)
    }

    func testNewCompositorNonceCannotInheritAnOldCallbackOrReusedSceneID() {
        let journal = PresentationJournal()
        let old = journal.register(key(1))
        XCTAssertTrue(old.claimSubmission())
        journal.retainSession(8)
        let current = journal.register(key(1, session: 8))
        XCTAssertTrue(current.claimSubmission())
        old.recordDrawableResult(presentedTime: 1)
        XCTAssertTrue(journal.results(sessionID: 8).isEmpty)
        XCTAssertTrue(journal.hasSubmitted(sessionID: 8))
        current.recordDrawableResult(presentedTime: 2)
        XCTAssertEqual(journal.results(sessionID: 8).first?.hostTimeNanoseconds, 2_000_000_000)
        XCTAssertTrue(journal.results(sessionID: 7).isEmpty)
    }

    func testPrecommitDiscardAndSubmissionHaveOneAtomicWinner() {
        let journal = PresentationJournal()
        for id in UInt32(1)...100 {
            let record = journal.register(key(id))
            DispatchQueue.concurrentPerform(iterations: 2) { index in
                if index == 0 {
                    if record.claimSubmission() { record.recordDrawableResult(presentedTime: 20) }
                } else { record.discardIfUnsubmitted() }
            }
            XCTAssertFalse(record.claimSubmission())
        }
        let results = journal.results(sessionID: 7)
        XCTAssertEqual(results.count, 100)
        XCTAssertTrue(results.allSatisfy { $0.hostTimeNanoseconds == 0 || $0.hostTimeNanoseconds == 20_000_000_000 })
        XCTAssertFalse(journal.hasSubmitted())
    }

    func testInvalidDrawableTimeRemainsUnknownButActualZeroIsTerminal() {
        let journal = PresentationJournal()
        let record = journal.register(key(1))
        XCTAssertTrue(record.claimSubmission())
        XCTAssertFalse(record.hasDiscardResult)
        for invalid in [Double.nan, Double.infinity, -1, Double(UInt64.max), Double.leastNonzeroMagnitude] {
            record.recordDrawableResult(presentedTime: invalid)
            XCTAssertFalse(record.hasDiscardResult)
            XCTAssertTrue(journal.hasSubmitted())
            XCTAssertTrue(journal.results(sessionID: 7).isEmpty)
        }
        record.recordDrawableResult(presentedTime: 0)
        XCTAssertTrue(record.hasDiscardResult)
        XCTAssertFalse(journal.hasSubmitted())
        XCTAssertEqual(journal.results(sessionID: 7).map(\.hostTimeNanoseconds), [0])
    }
}
