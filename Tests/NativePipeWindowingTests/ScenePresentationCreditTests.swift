import XCTest
@testable import NativePipeWindowing

final class ScenePresentationCreditTests: XCTestCase {
    @MainActor func testActualTimestampSurvivesOneShotCompletionWithoutTransferToNewScene() {
        let credits = ScenePresentationCredits()
        var old: [(Bool, Double?)] = []
        var current: [(Bool, Double?)] = []
        let first = credits.registerTimed(1) { old.append(($0, $1)) }
        // Superseding a pending scene retires its own actual-display feedback;
        // the replacement does not inherit the old scene's presentation time.
        first.finish(false)
        let next = credits.registerTimed(2) { current.append(($0, $1)) }
        first.finish(true, presentedTime: 100)
        next.finish(true, presentedTime: 102.125)
        next.finish(false)
        credits.discardUnpresented()
        XCTAssertEqual(old.count, 1)
        XCTAssertFalse(old[0].0)
        XCTAssertNil(old[0].1)
        XCTAssertEqual(current.count, 1)
        XCTAssertTrue(current[0].0)
        XCTAssertEqual(current[0].1, 102.125)
    }
    @MainActor func testOccludedUnreportedDrawableCreditsRemainDeferredUntilCaptureFlush() {
        let credits = ScenePresentationCredits()
        var feedback = DeferredSceneFeedback()
        var sent: [UInt32] = []
        let callbacks = (1...8).map { index in
            let id = UInt32(index)
            return credits.register(id) { displayed in
                if feedback.shouldSend(surface: 1, presentationID: id,
                    displayed: displayed, occluded: true) { sent.append(id) }
            }
        }
        // All eight scenes completed their reads, but none reported a drawable
        // presentation. Hiding retires them without unthrottling the guest.
        credits.discardUnpresented()
        XCTAssertTrue(sent.isEmpty)
        let explicitCaptureFlush = feedback.take(surface: 1)
        XCTAssertEqual(explicitCaptureFlush.sorted(), Array(1...8).map(UInt32.init))
        sent.append(contentsOf: explicitCaptureFlush)

        for callback in callbacks { callback.finish(true) }
        XCTAssertEqual(sent.count, 8, "Late drawable callbacks cannot return NPRP twice")
        XCTAssertTrue(feedback.take(surface: 1).isEmpty)
    }

    @MainActor func testRestoredVisibilityRetiresOutstandingDrawableCreditsOnlyOnce() {
        let credits = ScenePresentationCredits()
        var feedback = DeferredSceneFeedback()
        var sent: [(UInt32, Bool)] = []
        let callbacks = (1...8).map { index in
            let id = UInt32(index)
            return credits.register(id) { displayed in
                if feedback.shouldSend(surface: 1, presentationID: id,
                    displayed: displayed, occluded: false) { sent.append((id, displayed)) }
            }
        }
        credits.discardUnpresented()
        XCTAssertEqual(sent.map(\.0).sorted(), Array(1...8).map(UInt32.init))
        XCTAssertTrue(sent.allSatisfy { !$0.1 })
        XCTAssertTrue(feedback.take(surface: 1).isEmpty)
        for callback in callbacks { callback.finish(true) }
        credits.discardUnpresented()
        XCTAssertEqual(sent.count, 8)
    }

    @MainActor func testClosingOneWindowDoesNotRetireAnotherWindowsPresentation() {
        let firstWindow = ScenePresentationCredits()
        let secondWindow = ScenePresentationCredits()
        var first: [Bool] = []
        var second: [Bool] = []
        let firstCallback = firstWindow.register(1) { first.append($0) }
        let secondCallback = secondWindow.register(1) { second.append($0) }
        firstWindow.discardUnpresented()
        XCTAssertEqual(first, [false])
        XCTAssertTrue(second.isEmpty)
        firstCallback.finish(true)
        secondCallback.finish(true)
        secondWindow.discardUnpresented()
        XCTAssertEqual(first, [false])
        XCTAssertEqual(second, [true])
    }

    @MainActor func testRetiredCallbacksCannotAffectRemappedPresentationID() {
        let credits = ScenePresentationCredits()
        var oldFeedback: [Bool] = []
        var newFeedback: [Bool] = []
        let oldCallback = credits.register(1) { oldFeedback.append($0) }
        credits.discardUnpresented()
        let remappedCallback = credits.register(1) { newFeedback.append($0) }
        oldCallback.finish(true)
        XCTAssertEqual(oldFeedback, [false])
        XCTAssertTrue(newFeedback.isEmpty)
        credits.discardUnpresented()
        remappedCallback.finish(true)
        XCTAssertEqual(newFeedback, [false])
    }
}
