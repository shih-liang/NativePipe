import AppKit
import Foundation
import XCTest
import Metal
import QuartzCore
import NativePipeProtocol
@testable import NativePipeWindowing

/// Model Core Animation's unavailable drawable without opening a screen window.
private final class UnavailableCaptureLayer: CAMetalLayer, @unchecked Sendable {
    private let state = NSLock()
    private var entered = false
    private var calls = 0
    private var pausedAttempt = 1
    let resume = DispatchSemaphore(value: 0)

    var isWaiting: Bool {
        state.lock(); defer { state.unlock() }
        return entered
    }

    var attemptCount: Int {
        state.lock(); defer { state.unlock() }
        return calls
    }

    func pauseNextAttempt() {
        state.lock(); defer { state.unlock() }
        entered = false
        pausedAttempt = calls + 1
    }

    override func nextDrawable() -> CAMetalDrawable? {
        state.lock()
        calls += 1
        let paused = calls == pausedAttempt
        if paused { entered = true }
        state.unlock()
        if paused { _ = resume.wait(timeout: .now() + 5) }
        return nil
    }
}

private final class CaptureRetryProbe: @unchecked Sendable {
    private let lock = NSLock()
    private var value = 0
    var count: Int { lock.lock(); defer { lock.unlock() }; return value }
    func mark() { lock.lock(); value += 1; lock.unlock() }
}

final class CapturePresentationTests: XCTestCase {
    @MainActor func testNilDrawableCaptureRendersLatestResizedSceneWithoutPresentation() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw XCTSkip("Metal unavailable")
        }
        let layer = UnavailableCaptureLayer()
        layer.pixelFormat = .bgra8Unorm
        let renderer = try HostSceneRenderer(device: device)
        var offscreenCompletions = 0
        let presenter = AsyncMetalScenePresenter(layer: layer, device: device, renderer: renderer,
            offscreenCaptureCompleted: { offscreenCompletions += 1 })
        defer { layer.resume.signal(); presenter.cancelPending() }

        var captured: Result<RenderedFrameCapture, Error>?
        var readSuccess: [Int] = []
        var discarded: [Int] = []
        var latched: [Int] = []
        var presented: [Bool] = []
        var timedDiscards: [Int] = []
        var timedPresentations: [Double] = []
        let journal = PresentationJournal()
        var records: [PresentationRecord] = []
        _ = presenter.captureNextFrame { captured = $0 }

        func enqueue(_ index: Int, width: Int = 64, height: Int = 48) throws {
            let (scene, layers) = try makeScene(
                device: device, index: index, width: width, height: height)
            let record = journal.register(.init(
                sessionID: 1, clockEpoch: 1, surface: 1, presentationID: UInt32(index)))
            records.append(record)
            presenter.enqueue(scene: scene, layers: layers,
                drawableSize: CGSize(width: width, height: height),
                readComplete: { success in
                    if success { readSuccess.append(index) } else { discarded.append(index) }
                }, latched: { latched.append(index) },
                presented: { presented.append($0) },
                presentationTime: { time in
                    if let time { timedPresentations.append(time) }
                    else { timedDiscards.append(index) }
                }, record: record)
        }

        try enqueue(1)
        for _ in 0..<300 {
            if layer.isWaiting { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        guard layer.isWaiting else {
            XCTFail("Presenter did not request a drawable"); return
        }
        for index in 2...7 { try enqueue(index) }
        try enqueue(8, width: 96, height: 72)
        layer.resume.signal()
        for _ in 0..<600 {
            if captured != nil, readSuccess.count + discarded.count == 8,
               latched.count == 8, presented.count == 1, offscreenCompletions == 1 { break }
            try await Task.sleep(for: .milliseconds(5))
        }

        let capture = try XCTUnwrap(captured).get()
        XCTAssertEqual(capture.width, 96)
        XCTAssertEqual(capture.height, 72)
        XCTAssertEqual(Array(capture.pixels.prefix(4)), [160, 160, 160, 255])
        // A scaled, half-opacity overlay must be composed over the base layer;
        // returning that base texture directly would pass the corner assertion.
        let overlayOffset = 12 * capture.bytesPerRow + 16 * 4
        let overlayPixel = Array(capture.pixels[overlayOffset..<(overlayOffset + 4)])
        for (actual, expected) in zip(overlayPixel, [85, 100, 180, 255]) {
            XCTAssertEqual(Double(actual), Double(expected), accuracy: 1)
        }
        XCTAssertEqual(readSuccess, [8], "Only the latest protected source is read by Metal")
        XCTAssertEqual(discarded.sorted(), Array(1...7))
        XCTAssertEqual(latched.sorted(), Array(1...8), "All superseded latch obligations survive")
        XCTAssertEqual(presented, [false], "An offscreen capture cannot claim screen presentation")
        XCTAssertEqual(timedDiscards.sorted(), Array(1...8),
                       "Every superseded scene keeps its own discarded outcome")
        XCTAssertTrue(timedPresentations.isEmpty,
                      "GPU read completion and an offscreen capture never invent an actual display time")
        let actualResults = journal.results(sessionID: 1)
        XCTAssertEqual(actualResults.map(\.key.presentationID).sorted(), Array(1...8).map(UInt32.init))
        XCTAssertTrue(actualResults.allSatisfy { $0.hostTimeNanoseconds == 0 })
        XCTAssertFalse(journal.hasSubmitted(sessionID: 1))
        for record in records { XCTAssertFalse(record.claimSubmission()) }
        XCTAssertEqual(offscreenCompletions, 1, "A completed read enables one static-scene refresh")
        XCTAssertEqual(layer.attemptCount, 1)
    }

    @MainActor func testCancelledCaptureKeepsNilDrawableOnDisplayRetryPath() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw XCTSkip("Metal unavailable")
        }
        let layer = UnavailableCaptureLayer()
        layer.pixelFormat = .bgra8Unorm
        let renderer = try HostSceneRenderer(device: device)
        let retry = expectation(description: "Ordinary scene waits for the display retry")
        let presenter = AsyncMetalScenePresenter(layer: layer, device: device,
            renderer: renderer, requestDisplayRetry: { retry.fulfill() })
        defer { layer.resume.signal(); presenter.cancelPending() }

        var captures = 0
        var reads: [Bool] = []
        var latches = 0
        let captureID = presenter.captureNextFrame { _ in captures += 1 }
        presenter.cancelCapture(captureID)
        let (scene, layers) = try makeScene(device: device, index: 1, width: 64, height: 48)
        presenter.enqueue(scene: scene, layers: layers,
            drawableSize: CGSize(width: 64, height: 48),
            readComplete: { reads.append($0) }, latched: { latches += 1 })
        layer.resume.signal()
        await fulfillment(of: [retry], timeout: 2)
        XCTAssertEqual(layer.attemptCount, 1)
        XCTAssertTrue(reads.isEmpty, "Ordinary scenes retain their source for a display retry")
        XCTAssertEqual(latches, 0)
        XCTAssertEqual(captures, 0)

        presenter.cancelPending()
        for _ in 0..<300 {
            if reads.count == 1, latches == 1 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertEqual(reads, [false])
        XCTAssertEqual(latches, 1)
        XCTAssertEqual(captures, 0)
    }

    @MainActor func testCaptureWakesProtectedSceneWaitingForDisplayRetryWithoutNewScene() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let layer = UnavailableCaptureLayer()
        layer.pixelFormat = .bgra8Unorm
        let renderer = try HostSceneRenderer(device: device)
        let retry = CaptureRetryProbe()
        let presenter = AsyncMetalScenePresenter(layer: layer, device: device,
            renderer: renderer, requestDisplayRetry: { retry.mark() })
        defer { layer.resume.signal(); presenter.cancelPending() }
        var reads: [Bool] = []
        var latches = 0
        var captured: Result<RenderedFrameCapture, Error>?
        let (scene, layers) = try makeScene(device: device, index: 1, width: 64, height: 48)
        presenter.enqueue(scene: scene, layers: layers,
            drawableSize: CGSize(width: 64, height: 48),
            readComplete: { reads.append($0) }, latched: { latches += 1 })
        layer.resume.signal()
        for _ in 0..<300 {
            if retry.count == 1 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertEqual(retry.count, 1)
        XCTAssertTrue(reads.isEmpty)

        // No display tick or newer guest scene follows the capture request.
        _ = presenter.captureNextFrame { captured = $0 }
        for _ in 0..<300 {
            if captured != nil, reads.count == 1, latches == 1 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        let image = try XCTUnwrap(captured).get()
        XCTAssertEqual(image.width, 64)
        XCTAssertEqual(image.height, 48)
        XCTAssertEqual(Array(image.pixels.prefix(4)), [20, 20, 20, 255])
        XCTAssertEqual(reads, [true])
        XCTAssertEqual(latches, 1)
        XCTAssertEqual(layer.attemptCount, 2)
    }

    @MainActor func testCancelledCaptureWakeDoesNotRenderOrdinaryNilDrawableScene() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let layer = UnavailableCaptureLayer()
        layer.pixelFormat = .bgra8Unorm
        let renderer = try HostSceneRenderer(device: device)
        let retry = CaptureRetryProbe()
        var offscreenCompletions = 0
        let presenter = AsyncMetalScenePresenter(layer: layer, device: device,
            renderer: renderer, requestDisplayRetry: { retry.mark() },
            offscreenCaptureCompleted: { offscreenCompletions += 1 })
        defer { layer.resume.signal(); presenter.cancelPending() }
        var reads: [Bool] = []
        var latches = 0
        var captures = 0
        let (scene, layers) = try makeScene(device: device, index: 1, width: 64, height: 48)
        presenter.enqueue(scene: scene, layers: layers,
            drawableSize: CGSize(width: 64, height: 48),
            readComplete: { reads.append($0) }, latched: { latches += 1 })
        layer.resume.signal()
        for _ in 0..<300 {
            if retry.count == 1 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertEqual(retry.count, 1)
        layer.pauseNextAttempt()
        let requestID = presenter.captureNextFrame { _ in captures += 1 }
        for _ in 0..<300 {
            if layer.isWaiting { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(layer.isWaiting)
        presenter.cancelCapture(requestID)
        layer.resume.signal()
        for _ in 0..<300 {
            if retry.count == 2 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertEqual(retry.count, 2)
        XCTAssertTrue(reads.isEmpty)
        XCTAssertEqual(latches, 0)
        XCTAssertEqual(captures, 0)
        XCTAssertEqual(offscreenCompletions, 0)
    }

    @MainActor func testDiscardedRecordRejectsFinalDrawableCommitAndCompletesReadsAndLatches() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let layer = GatedLayer()
        layer.pixelFormat = .bgra8Unorm
        let renderer = try HostSceneRenderer(device: device)
        let presenter = AsyncMetalScenePresenter(layer: layer, device: device, renderer: renderer)
        defer { layer.resume.signal(); presenter.cancelPending() }
        let journal = PresentationJournal()
        let record = journal.register(.init(sessionID: 1, clockEpoch: 1, surface: 1, presentationID: 1))
        var reads: [Bool] = []
        var latches = 0
        var captured: Result<RenderedFrameCapture, Error>?
        _ = presenter.captureNextFrame { captured = $0 }
        let (scene, layers) = try makeScene(device: device, index: 1, width: 64, height: 48)
        presenter.enqueue(scene: scene, layers: layers, drawableSize: CGSize(width: 64, height: 48),
            readComplete: { reads.append($0) }, latched: { latches += 1 }, record: record)
        for _ in 0..<300 {
            if layer.isWaiting { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(layer.isWaiting)
        XCTAssertTrue(record.discardIfUnsubmitted())
        layer.resume.signal()
        await withCheckedContinuation { continuation in
            presenter.fenceSubmission { continuation.resume() }
        }
        XCTAssertEqual(layer.drawableCount, 1, "The rejection must run after acquiring a real drawable")
        XCTAssertEqual(reads, [false], "A rejected commit releases sources without reporting a GPU read")
        XCTAssertEqual(latches, 1)
        guard case .failure? = captured else { return XCTFail("A rejected commit must fail its capture") }
        XCTAssertEqual(journal.results(sessionID: 1).map(\.hostTimeNanoseconds), [0])
        XCTAssertFalse(journal.hasSubmitted(sessionID: 1))
        record.recordDrawableResult(presentedTime: 100)
        XCTAssertEqual(journal.results(sessionID: 1).map(\.hostTimeNanoseconds), [0])
    }

    @MainActor func testClosedSubmissionGateFencesCanceledLatchesAndRejectsCapture() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let layer = UnavailableCaptureLayer()
        layer.pixelFormat = .bgra8Unorm
        let renderer = try HostSceneRenderer(device: device)
        let presenter = AsyncMetalScenePresenter(layer: layer, device: device, renderer: renderer)
        defer { layer.resume.signal(); presenter.cancelPending() }
        let journal = PresentationJournal()
        let record = journal.register(.init(sessionID: 1, clockEpoch: 1, surface: 1, presentationID: 1))
        var reads: [Bool] = []
        var latches = 0
        var captured: Result<RenderedFrameCapture, Error>?
        _ = presenter.captureNextFrame { captured = $0 }
        let (scene, layers) = try makeScene(device: device, index: 1, width: 64, height: 48)
        presenter.enqueue(scene: scene, layers: layers, drawableSize: CGSize(width: 64, height: 48),
            readComplete: { reads.append($0) }, latched: { latches += 1 }, record: record)
        for _ in 0..<300 {
            if layer.isWaiting { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(layer.isWaiting)
        presenter.setSubmissionAllowed(false)
        layer.resume.signal()
        await withCheckedContinuation { continuation in
            presenter.fenceSubmission { continuation.resume() }
        }
        XCTAssertEqual(reads, [false])
        XCTAssertEqual(latches, 1, "The fence follows the worker's queued latch")
        guard case .failure? = captured else { return XCTFail("Closing the gate must fail its queued capture") }
        XCTAssertEqual(journal.results(sessionID: 1).map(\.hostTimeNanoseconds), [0])
        XCTAssertFalse(journal.hasSubmitted(sessionID: 1))

        var rejectedCapture: Result<RenderedFrameCapture, Error>?
        _ = presenter.captureNextFrame { rejectedCapture = $0 }
        await withCheckedContinuation { continuation in
            presenter.fenceSubmission { continuation.resume() }
        }
        guard case .failure? = rejectedCapture else { return XCTFail("A closed gate cannot enqueue a new capture") }
        XCTAssertEqual(layer.attemptCount, 1)
    }

    @MainActor func testHiddenWindowCapturesOffscreenAndDiscardsOrdinarySceneWithoutDrawableRetry() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let layer = UnavailableCaptureLayer()
        layer.pixelFormat = .bgra8Unorm
        let renderer = try HostSceneRenderer(device: device)
        let retries = CaptureRetryProbe()
        let presenter = AsyncMetalScenePresenter(layer: layer, device: device, renderer: renderer,
            requestDisplayRetry: { retries.mark() })
        defer { presenter.cancelPending() }
        let journal = PresentationJournal()
        let record = journal.register(.init(sessionID: 1, clockEpoch: 1, surface: 1, presentationID: 1))
        var captured: Result<RenderedFrameCapture, Error>?
        var reads: [Bool] = []
        var latches = 0
        _ = presenter.captureNextFrame { captured = $0 }
        presenter.setDrawableAllowed(false)
        let (scene, layers) = try makeScene(device: device, index: 1, width: 64, height: 48)
        presenter.enqueue(scene: scene, layers: layers, drawableSize: CGSize(width: 64, height: 48),
            readComplete: { reads.append($0) }, latched: { latches += 1 }, record: record)
        for _ in 0..<300 {
            if captured != nil, reads.count == 1, latches == 1 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        let image = try XCTUnwrap(captured).get()
        XCTAssertEqual(Array(image.pixels.prefix(4)), [20, 20, 20, 255])
        XCTAssertEqual(reads, [true])
        XCTAssertEqual(latches, 1)
        XCTAssertEqual(layer.attemptCount, 0, "Hidden captures must not request a screen drawable")
        XCTAssertEqual(journal.results(sessionID: 1).map(\.hostTimeNanoseconds), [0])
        XCTAssertFalse(journal.hasSubmitted(sessionID: 1))

        let ordinary = journal.register(.init(sessionID: 1, clockEpoch: 1, surface: 1, presentationID: 2))
        let (secondScene, secondLayers) = try makeScene(device: device, index: 2, width: 64, height: 48)
        presenter.enqueue(scene: secondScene, layers: secondLayers, drawableSize: CGSize(width: 64, height: 48),
            readComplete: { reads.append($0) }, latched: { latches += 1 }, record: ordinary)
        await withCheckedContinuation { continuation in
            presenter.fenceSubmission { continuation.resume() }
        }
        XCTAssertEqual(reads, [true, false])
        XCTAssertEqual(latches, 2)
        XCTAssertEqual(layer.attemptCount, 0)
        XCTAssertEqual(retries.count, 0)
        XCTAssertEqual(journal.results(sessionID: 1).map(\.hostTimeNanoseconds), [0, 0])
    }

    @MainActor func testEncoderCommitHookRejectionDoesNotSubmitCapture() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let renderer = try HostSceneRenderer(device: device)
        let hook = CaptureRetryProbe()
        let completed = CaptureRetryProbe()
        let (scene, layers) = try makeScene(device: device, index: 1, width: 64, height: 48)
        XCTAssertThrowsError(try renderer.encodeCapture(scene: scene, layers: layers,
            willCommitDrawable: { hook.mark(); return false },
            completion: { _, _ in completed.mark() })) { error in
                guard let rendererError = error as? HostSceneRenderer.RendererError,
                      case .cancelled = rendererError else {
                    return XCTFail("Expected commit rejection, received \(error)")
                }
            }
        XCTAssertEqual(hook.count, 1)
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertEqual(completed.count, 0, "An uncommitted command cannot complete a GPU read")
    }

    @MainActor func testRetiringDisplayCreditDoesNotReleaseProtectedSceneBeforeGPUCompletion() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let layer = GatedLayer()
        layer.pixelFormat = .bgra8Unorm
        let renderer = try HostSceneRenderer(device: device)
        let presenter = AsyncMetalScenePresenter(layer: layer, device: device, renderer: renderer)
        defer { layer.resume.signal(); presenter.cancelPending() }
        let credits = ScenePresentationCredits()
        var displayFeedback: [Bool] = []
        let completion = credits.register(1) { displayFeedback.append($0) }
        var reads: [Bool] = []
        var captured: Result<RenderedFrameCapture, Error>?
        weak var protectedOwner: NSObject?
        _ = presenter.captureNextFrame { captured = $0 }

        func enqueueProtectedScene() throws {
            let owner = NSObject()
            protectedOwner = owner
            let (scene, layers) = try makeScene(device: device, index: 1, width: 64, height: 48)
            let protected = layers.map {
                ResolvedSceneLayer(state: $0.state, texture: $0.texture, owner: owner)
            }
            presenter.enqueue(scene: scene, layers: protected,
                drawableSize: CGSize(width: 64, height: 48),
                readComplete: { reads.append($0) }, latched: {},
                // Model a missing/late presentation report independently of
                // this real drawable's Metal read completion.
                presented: { _ in })
        }
        try enqueueProtectedScene()
        for _ in 0..<300 {
            if layer.isWaiting { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(layer.isWaiting)
        XCTAssertNotNil(protectedOwner)
        credits.discardUnpresented()
        XCTAssertEqual(displayFeedback, [false])
        XCTAssertTrue(reads.isEmpty, "Display-credit retirement cannot release protected source reads")
        XCTAssertNotNil(protectedOwner)

        layer.resume.signal()
        for _ in 0..<300 {
            if captured != nil, reads.count == 1 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        let image = try XCTUnwrap(captured).get()
        XCTAssertEqual(Array(image.pixels.prefix(4)), [20, 20, 20, 255])
        XCTAssertEqual(reads, [true])
        XCTAssertEqual(layer.drawableCount, 1, "This regression uses a real non-nil drawable")
        completion.finish(true)
        XCTAssertEqual(displayFeedback, [false])
    }

    func testOffscreenSceneInvalidationRestoresSkippedDamageOnNextDrawable() {
        var tracker = DrawableAgeTracker(capacity: 3)
        var displayed = Windowing.SceneSnapshot(
            surface: 1, presentationID: 1, width: 64, height: 48, scale: 1,
            windowGeometry: .init(x: 0, y: 0, width: 64, height: 48), layers: [])
        tracker.commit(tracker.plan(
            drawableID: 10, scene: displayed, drawableWidth: 64, drawableHeight: 48))

        // B changes R1 while occluded and is captured into a private target.
        // C changes R2 after B; presentation-id gaps do not restore R1 alone.
        var offscreen = displayed
        offscreen.presentationID = 2
        offscreen.damage = [.init(x: 1, y: 1, width: 4, height: 4)]
        displayed.presentationID = 3
        displayed.damage = [.init(x: 40, y: 30, width: 4, height: 4)]
        let stale = tracker.plan(
            drawableID: 10, scene: displayed, drawableWidth: 64, drawableHeight: 48)
        XCTAssertFalse(stale.redrawAll)
        XCTAssertEqual(stale.damage, displayed.damage)
        XCTAssertFalse(stale.damage.contains(offscreen.damage[0]))

        tracker.invalidate()
        let restored = tracker.plan(
            drawableID: 10, scene: displayed, drawableWidth: 64, drawableHeight: 48)
        XCTAssertTrue(restored.redrawAll)
        XCTAssertEqual(restored.damage, [.init(x: 0, y: 0, width: 64, height: 48)])
    }

    func testStaticOffscreenSceneRefreshWaitsForVisibilityAndRunsOnce() {
        var recovery = SceneCaptureRecovery()
        recovery.needsSceneRefreshAfterCapture = true
        XCTAssertFalse(recovery.takeRefresh(canPresent: false))
        XCTAssertTrue(recovery.needsSceneRefreshAfterCapture)
        XCTAssertTrue(recovery.takeRefresh(canPresent: true))
        XCTAssertFalse(recovery.takeRefresh(canPresent: true))
    }

    func testVisibilityBeforeOffscreenCompletionStillRefreshesTheStaticScene() {
        var recovery = SceneCaptureRecovery()
        XCTAssertFalse(recovery.takeRefresh(canPresent: true))
        recovery.needsSceneRefreshAfterCapture = true
        XCTAssertTrue(recovery.takeRefresh(canPresent: true))
        XCTAssertFalse(recovery.takeRefresh(canPresent: true))

        // Closing the window clears the obligation instead of reopening it.
        recovery.needsSceneRefreshAfterCapture = true
        recovery.needsSceneRefreshAfterCapture = false
        XCTAssertFalse(recovery.takeRefresh(canPresent: true))
    }

    private func makeScene(
        device: MTLDevice, index: Int, width: Int, height: Int
    ) throws -> (Windowing.SceneSnapshot, [ResolvedSceneLayer]) {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderRead]
        let texture = try XCTUnwrap(device.makeTexture(descriptor: descriptor))
        var bytes = [UInt8](repeating: UInt8(index * 20), count: width * height * 4)
        for offset in stride(from: 3, to: bytes.count, by: 4) { bytes[offset] = 255 }
        bytes.withUnsafeBytes { raw in
            texture.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0,
                withBytes: raw.baseAddress!, bytesPerRow: width * 4)
        }
        let bounds = Windowing.Rect(x: 0, y: 0, width: width, height: height)
        let sampleBounds = Windowing.FloatRect(
            x: 0, y: 0, width: Double(width), height: Double(height))
        let state = Windowing.SceneLayer(
            surface: 1, resourceID: UInt32(index), width: width, height: height,
            bytesPerRow: width * 4, format: .bgra8888,
            destination: sampleBounds, sourcePixels: sampleBounds, clip: sampleBounds,
            opaque: true)
        var resolved = [ResolvedSceneLayer(state: state, texture: texture)]
        if index == 8 {
            let overlayDescriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm, width: 16, height: 12, mipmapped: false)
            overlayDescriptor.storageMode = .shared
            overlayDescriptor.usage = [.shaderRead]
            let overlayTexture = try XCTUnwrap(device.makeTexture(descriptor: overlayDescriptor))
            let overlayPixel: [UInt8] = [10, 40, 200, 255]
            let overlayBytes = Array(repeating: overlayPixel, count: 16 * 12)
                .flatMap { $0 }
            overlayBytes.withUnsafeBytes { raw in
                overlayTexture.replace(region: MTLRegionMake2D(0, 0, 16, 12), mipmapLevel: 0,
                    withBytes: raw.baseAddress!, bytesPerRow: 16 * 4)
            }
            let overlay = Windowing.SceneLayer(
                surface: 2, resourceID: 100, width: 16, height: 12,
                bytesPerRow: 16 * 4, format: .bgra8888,
                destination: .init(x: 8, y: 6, width: 32, height: 24),
                sourcePixels: .init(x: 0, y: 0, width: 16, height: 12),
                clip: sampleBounds, alpha: 0.5, opaque: true)
            resolved.append(.init(state: overlay, texture: overlayTexture))
        }
        let scene = Windowing.SceneSnapshot(
            surface: 1, presentationID: UInt32(index), width: width, height: height,
            scale: 1, windowGeometry: bounds, layers: resolved.map(\.state), damage: [bounds])
        return (scene, resolved)
    }
}
