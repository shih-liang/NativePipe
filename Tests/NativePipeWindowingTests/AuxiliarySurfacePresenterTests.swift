import AppKit
import Metal
import NativePipeProtocol
import QuartzCore
import XCTest
@testable import NativePipeWindowing

private final class AuxiliaryWaitingLayer: CAMetalLayer, @unchecked Sendable {
    private let lock = NSLock()
    private var calls = 0
    let release = DispatchSemaphore(value: 0)
    var attemptCount: Int { lock.lock(); defer { lock.unlock() }; return calls }

    override func nextDrawable() -> CAMetalDrawable? {
        lock.lock()
        calls += 1
        let first = calls == 1
        lock.unlock()
        if first { _ = release.wait(timeout: .now() + 5) }
        return nil
    }
}

private final class AuxiliaryTextureOwner {}

@MainActor
private final class AuxiliaryReadback {
    var pixels: RenderedFrameCapture?
}

final class AuxiliarySurfacePresenterTests: XCTestCase {
    @MainActor func testViewportSamplingKeepsSurfaceIdentityAndUsesDisplayPixelDensity() throws {
        let frame = Windowing.Frame(resourceID: 23, width: 128, height: 96,
            bytesPerRow: 128 * 4, format: .bgra8888, scale: 2, presentationID: 19,
            viewportSource: .init(x: 8, y: 4, width: 16, height: 8),
            viewportDestination: .init(width: 12, height: 6))
        let geometry = try XCTUnwrap(AuxiliarySurfacePresenter.drawingGeometry(
            frame: frame, surface: 97, logicalSize: CGSize(width: 24, height: 12), backingScale: 2))
        XCTAssertEqual(geometry.drawableSize, CGSize(width: 48, height: 24))
        XCTAssertEqual(geometry.scene.surface, 97)
        XCTAssertEqual(geometry.scene.presentationID, 19)
        XCTAssertEqual(geometry.scene.layers[0].surface, 97)
        XCTAssertEqual(geometry.scene.layers[0].sourcePixels,
            .init(x: 16, y: 8, width: 32, height: 16))
        XCTAssertEqual(geometry.scene.layers[0].destination,
            .init(x: 0, y: 0, width: 48, height: 24))
        XCTAssertFalse(geometry.scene.layers[0].opaque)
    }

    @MainActor func testCursorHotspotUsesTopDownCoordinatesAndDragKeepsPointerOffset() {
        let pointer = CGPoint(x: 300, y: 400), size = CGSize(width: 32, height: 24)
        XCTAssertEqual(AuxiliarySurfacePresenter.origin(kind: .cursor, pointer: pointer,
            logicalSize: size, hotSpot: CGPoint(x: 5, y: 7)), CGPoint(x: 295, y: 383))
        XCTAssertEqual(AuxiliarySurfacePresenter.origin(kind: .drag, pointer: pointer,
            logicalSize: size, hotSpot: .zero), CGPoint(x: 308, y: 368))
    }

    @MainActor func testHiddenSurfaceDiscardsWithoutRequestingDrawableAndFinishesLatch() async throws {
        _ = NSApplication.shared
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let layer = AuxiliaryWaitingLayer()
        let auxiliary = AuxiliarySurfacePresenter(kind: .cursor, displayClock: DisplayClock(), metalLayer: layer)
        defer { layer.release.signal(); auxiliary.close() }
        let renderer = try HostSceneRenderer(device: device)
        let (frame, texture) = try source(device: device)
        let journal = PresentationJournal()
        let record = journal.register(.init(sessionID: 7, clockEpoch: 1, surface: 97, presentationID: 19))
        var reads: [Bool] = [], latches = 0, fenced = false
        XCTAssertTrue(auxiliary.present(texture: texture, owner: nil, frame: frame,
            surface: 97, logicalSize: CGSize(width: 24, height: 12), renderer: renderer,
            record: record, readComplete: { reads.append($0) }, latched: { latches += 1 }))
        auxiliary.fenceSubmission { fenced = true }
        try await until { fenced }
        XCTAssertEqual(reads, [false])
        XCTAssertEqual(latches, 1)
        XCTAssertEqual(layer.attemptCount, 0)
        XCTAssertEqual(journal.results(sessionID: 7).map(\.hostTimeNanoseconds), [0])
        XCTAssertFalse(journal.hasSubmitted())
    }

    @MainActor func testHideCancelsWaitingSceneRetainsSourceUntilWorkerFencesAndCompletesOnce() async throws {
        _ = NSApplication.shared
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let layer = AuxiliaryWaitingLayer()
        let auxiliary = AuxiliarySurfacePresenter(kind: .drag, displayClock: DisplayClock(), metalLayer: layer)
        defer { layer.release.signal(); auxiliary.close() }
        auxiliary.move(to: NSEvent.mouseLocation)
        auxiliary.setVisible(true)
        let renderer = try HostSceneRenderer(device: device)
        let (frame, texture) = try source(device: device)
        let journal = PresentationJournal()
        let record = journal.register(.init(sessionID: 7, clockEpoch: 1, surface: 97, presentationID: 19))
        var owner: AuxiliaryTextureOwner? = AuxiliaryTextureOwner()
        weak var retainedOwner = owner
        var reads: [Bool] = [], latches = 0, fenced = false
        XCTAssertTrue(auxiliary.present(texture: texture, owner: owner, frame: frame,
            surface: 97, logicalSize: CGSize(width: 24, height: 12), renderer: renderer,
            record: record, readComplete: { reads.append($0) }, latched: { latches += 1 }))
        owner = nil
        try await until { layer.attemptCount == 1 }
        auxiliary.setVisible(false)
        XCTAssertFalse(auxiliary.metalLayer === layer, "Hidden content must use a fresh drawable pool")
        XCTAssertNotNil(retainedOwner, "A selected source remains protected while its worker is waiting")
        layer.release.signal()
        auxiliary.fenceSubmission { fenced = true }
        try await until { fenced }
        XCTAssertEqual(reads, [false])
        XCTAssertEqual(latches, 1)
        XCTAssertNil(retainedOwner, "No displayed-source cache survives completion")
        XCTAssertEqual(layer.attemptCount, 1)
        XCTAssertEqual(journal.results(sessionID: 7).map(\.hostTimeNanoseconds), [0])
        XCTAssertFalse(journal.hasSubmitted())
    }

    @MainActor func testResetFencesRetiredQueueEvenAfterAnotherPresenterIsCreated() async throws {
        _ = NSApplication.shared
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let oldLayer = AuxiliaryWaitingLayer()
        let auxiliary = AuxiliarySurfacePresenter(kind: .drag, displayClock: DisplayClock(), metalLayer: oldLayer)
        defer { oldLayer.release.signal(); auxiliary.close() }
        auxiliary.move(to: NSEvent.mouseLocation)
        auxiliary.setVisible(true)
        let renderer = try HostSceneRenderer(device: device)
        let (initialFrame, texture) = try source(device: device)
        var frame = initialFrame
        let journal = PresentationJournal()
        var reads: [Bool] = [], latches = 0, fenced = false
        let old = journal.register(.init(sessionID: 7, clockEpoch: 1, surface: 97, presentationID: 19))
        XCTAssertTrue(auxiliary.present(texture: texture, owner: nil, frame: frame,
            surface: 97, logicalSize: CGSize(width: 24, height: 12), renderer: renderer,
            record: old, readComplete: { reads.append($0) }, latched: { latches += 1 }))
        try await until { oldLayer.attemptCount == 1 }
        auxiliary.resetContent()
        XCTAssertFalse(auxiliary.metalLayer === oldLayer)
        auxiliary.setSubmissionAllowed(false)
        frame.presentationID = 20
        let current = journal.register(.init(sessionID: 7, clockEpoch: 1, surface: 98, presentationID: 20))
        XCTAssertTrue(auxiliary.present(texture: texture, owner: nil, frame: frame,
            surface: 98, logicalSize: CGSize(width: 24, height: 12), renderer: renderer,
            record: current, readComplete: { reads.append($0) }, latched: { latches += 1 }))
        auxiliary.fenceSubmission { fenced = true }
        try await Task.sleep(for: .milliseconds(30))
        XCTAssertFalse(fenced, "A new current queue cannot hide the retired worker's outstanding fence")
        oldLayer.release.signal()
        try await until { fenced }
        XCTAssertEqual(reads, [false, false])
        XCTAssertEqual(latches, 2)
        XCTAssertEqual(journal.results(sessionID: 7).map(\.hostTimeNanoseconds), [0, 0])
    }

    @MainActor func testRoleResetRendererReplacementAndCloseUseFreshLayers() async throws {
        _ = NSApplication.shared
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let auxiliary = AuxiliarySurfacePresenter(kind: .cursor, displayClock: DisplayClock())
        defer { auxiliary.close() }
        let renderer = try HostSceneRenderer(device: device)
        let replacementRenderer = try HostSceneRenderer(device: device)
        let (frame, texture) = try source(device: device)
        func present(using renderer: HostSceneRenderer) {
            XCTAssertTrue(auxiliary.present(texture: texture, owner: nil, frame: frame,
                surface: 97, logicalSize: CGSize(width: 24, height: 12), renderer: renderer,
                record: nil, readComplete: { _ in }, latched: {}))
        }
        present(using: renderer)
        let first = auxiliary.metalLayer
        present(using: replacementRenderer)
        XCTAssertFalse(auxiliary.metalLayer === first, "Different workers never share a drawable layer")
        let second = auxiliary.metalLayer
        auxiliary.resetContent()
        XCTAssertFalse(auxiliary.metalLayer === second, "A new role starts with an empty layer")
        let third = auxiliary.metalLayer
        auxiliary.close()
        XCTAssertFalse(auxiliary.metalLayer === third)
        var fenced = false
        auxiliary.fenceSubmission { fenced = true }
        try await until { fenced }
    }

    @MainActor func testBGRXPaddingByteNeverBecomesDrawableAlpha() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let renderer = try HostSceneRenderer(device: device)
        let (initialFrame, texture) = try source(device: device)
        var frame = initialFrame
        frame.format = .bgrx8888
        let bytes: [UInt8] = Array(repeating: [UInt8(24), 48, 96, 0], count: 48 * 24).flatMap { $0 }
        bytes.withUnsafeBytes {
            texture.replace(region: MTLRegionMake2D(0, 0, 48, 24), mipmapLevel: 0,
                withBytes: $0.baseAddress!, bytesPerRow: 48 * 4)
        }
        let geometry = try XCTUnwrap(AuxiliarySurfacePresenter.drawingGeometry(
            frame: frame, surface: 97, logicalSize: CGSize(width: 24, height: 12), backingScale: 2))
        XCTAssertTrue(geometry.scene.layers[0].opaque)
        let capture = AuxiliaryReadback()
        try renderer.encodeCapture(scene: geometry.scene,
            layers: [.init(state: geometry.scene.layers[0], texture: texture)]) { _, pixels in
                MainRunLoop.perform { capture.pixels = pixels }
            }
        try await until { capture.pixels != nil }
        XCTAssertEqual(Array(try XCTUnwrap(capture.pixels).pixels.prefix(4)), [24, 48, 96, 255])
    }

    @MainActor func testRuntimeCursorDrawableReportsPositiveActualTimeWithoutActivation() async throws {
        try await checkRuntimePresentation(kind: .cursor)
    }

    @MainActor func testRuntimeDragDrawableReportsPositiveActualTimeWithoutActivation() async throws {
        try await checkRuntimePresentation(kind: .drag)
    }

    /// Uses a real NSPanel and ordinary CAMetalLayer. The initial drawable may
    /// actually be skipped; preserve that zero and publish a fresh protected
    /// source instead of assuming the first attempt must reach the display.
    @MainActor private func checkRuntimePresentation(kind: AuxiliarySurfacePresenter.Kind) async throws {
        _ = NSApplication.shared
        let screen = try PhysicalDisplayTestSupport.requireVisibleDisplay()
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let auxiliary = AuxiliarySurfacePresenter(kind: kind, displayClock: DisplayClock())
        defer { auxiliary.onNeedsNewPublication = nil; auxiliary.close() }
        auxiliary.move(to: CGPoint(x: screen.visibleFrame.midX, y: screen.visibleFrame.midY),
            hotSpot: CGPoint(x: 5, y: 7))
        auxiliary.setVisible(true)
        let renderer = try HostSceneRenderer(device: device)
        let journal = PresentationJournal()
        var lastID: UInt32 = 18
        var ids: [UInt32] = []
        var reads: [UInt32: [Bool]] = [:], latches: [UInt32: Int] = [:]
        var phaseAttempts = 0
        var retryLimitReached = false
        var publicationError: Error?
        let before = UInt64((CACurrentMediaTime() * 1_000_000_000).rounded())
        func publish() throws {
            guard phaseAttempts < 5 else { retryLimitReached = true; return }
            phaseAttempts += 1
            lastID += 1
            let id = lastID
            ids.append(id)
            let (initialFrame, texture) = try source(device: device)
            var frame = initialFrame
            frame.presentationID = id
            let record = journal.register(.init(sessionID: 7, clockEpoch: 1,
                surface: 97, presentationID: id))
            XCTAssertTrue(auxiliary.present(texture: texture, owner: nil, frame: frame,
                surface: 97, logicalSize: CGSize(width: 24, height: 12), renderer: renderer,
                record: record,
                readComplete: { reads[id, default: []].append($0) },
                latched: { latches[id, default: 0] += 1 }))
            CATransaction.flush()
        }
        auxiliary.onNeedsNewPublication = { surface, id in
            XCTAssertEqual(surface, 97)
            XCTAssertEqual(id, lastID, "Only the current publication may request a successor")
            XCTAssertEqual(journal.results(sessionID: 7).first { $0.key.presentationID == id }?.hostTimeNanoseconds,
                0, "Republishing follows the actual skipped-drawable result")
            do { try publish() } catch { publicationError = error }
        }
        func phaseCompleted() -> Bool {
            if publicationError != nil || retryLimitReached { return true }
            let results = journal.results(sessionID: 7)
            return (results.last?.hostTimeNanoseconds ?? 0) > 0 && results.count == ids.count &&
                ids.allSatisfy { reads[$0]?.count == 1 && latches[$0] == 1 }
        }
        try publish()
        let panel = try XCTUnwrap(NSApp.windows.first { $0.contentView?.layer === auxiliary.metalLayer })
        XCTAssertFalse(panel.isKeyWindow)
        XCTAssertFalse(panel.isMainWindow)
        do {
            try await until(timeout: 6, phaseCompleted)
        } catch {
            _ = try PhysicalDisplayTestSupport.requireVisibleDisplay()
            throw error
        }
        _ = try PhysicalDisplayTestSupport.requireVisibleDisplay()
        if let publicationError { throw publicationError }
        XCTAssertFalse(retryLimitReached, "Five real publications must establish a displayed result")
        XCTAssertLessThanOrEqual(phaseAttempts, 5)
        let firstPhase = journal.results(sessionID: 7)
        XCTAssertEqual(firstPhase.map(\.key.presentationID), ids,
            "All outcomes, including an initial actual zero, remain recorded")
        let first = try XCTUnwrap(firstPhase.last)
        XCTAssertGreaterThan(first.hostTimeNanoseconds, 0, "The real drawable callback must report display")
        XCTAssertGreaterThanOrEqual(first.hostTimeNanoseconds, before)
        XCTAssertEqual(first.outputID, 0, "The moving overlay cannot prove which physical output displayed it")
        XCTAssertEqual(first.refreshNanoseconds, 0)
        for id in ids {
            XCTAssertEqual(reads[id], [true], "Every attempt reads its own protected source exactly once")
            XCTAssertEqual(latches[id], 1)
        }
        XCTAssertFalse(panel.isKeyWindow)
        XCTAssertFalse(panel.isMainWindow)

        let oldLayer = auxiliary.metalLayer
        auxiliary.setVisible(false)
        auxiliary.setVisible(true)
        XCTAssertFalse(auxiliary.metalLayer === oldLayer)
        XCTAssertFalse(panel.isVisible, "Old pixels cannot reappear before a fresh drawable is submitted")
        XCTAssertEqual(journal.results(sessionID: 7), firstPhase,
            "Hiding preserves both skipped and displayed facts from the original attempts")
        phaseAttempts = 0
        try publish()
        do {
            try await until(timeout: 6, phaseCompleted)
        } catch {
            _ = try PhysicalDisplayTestSupport.requireVisibleDisplay()
            throw error
        }
        _ = try PhysicalDisplayTestSupport.requireVisibleDisplay()
        if let publicationError { throw publicationError }
        XCTAssertFalse(retryLimitReached)
        XCTAssertLessThanOrEqual(phaseAttempts, 5)
        XCTAssertGreaterThan(try XCTUnwrap(journal.results(sessionID: 7).last).hostTimeNanoseconds,
            first.hostTimeNanoseconds)
        XCTAssertEqual(journal.results(sessionID: 7).map(\.key.presentationID), ids)
        for id in ids {
            XCTAssertEqual(reads[id], [true])
            XCTAssertEqual(latches[id], 1)
        }
        XCTAssertFalse(panel.isKeyWindow)
        XCTAssertFalse(panel.isMainWindow)
    }

    @MainActor func testClosedSubmissionGateFencesVisibleLatchesWithoutDrawableAttempt() async throws {
        _ = NSApplication.shared
        guard let device = MTLCreateSystemDefaultDevice() else { throw XCTSkip("Metal unavailable") }
        let layer = AuxiliaryWaitingLayer()
        let auxiliary = AuxiliarySurfacePresenter(kind: .cursor, displayClock: DisplayClock(), metalLayer: layer)
        defer { layer.release.signal(); auxiliary.close() }
        auxiliary.move(to: NSEvent.mouseLocation)
        auxiliary.setVisible(true)
        auxiliary.setSubmissionAllowed(false)
        let renderer = try HostSceneRenderer(device: device)
        let (frame, texture) = try source(device: device)
        let journal = PresentationJournal()
        let record = journal.register(.init(sessionID: 7, clockEpoch: 1, surface: 97, presentationID: 19))
        var reads: [Bool] = [], latches = 0, fenced = false
        XCTAssertTrue(auxiliary.present(texture: texture, owner: nil, frame: frame,
            surface: 97, logicalSize: CGSize(width: 24, height: 12), renderer: renderer,
            record: record, readComplete: { reads.append($0) }, latched: { latches += 1 }))
        auxiliary.fenceSubmission { XCTAssertEqual(latches, 1); fenced = true }
        try await until { fenced }
        XCTAssertEqual(reads, [false])
        XCTAssertEqual(latches, 1)
        XCTAssertEqual(layer.attemptCount, 0)
        XCTAssertEqual(journal.results(sessionID: 7).map(\.hostTimeNanoseconds), [0])
    }

    @MainActor private func source(device: MTLDevice) throws -> (Windowing.Frame, MTLTexture) {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
            width: 48, height: 24, mipmapped: false)
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderRead]
        let texture = try XCTUnwrap(device.makeTexture(descriptor: descriptor))
        let bytes: [UInt8] = Array(repeating: [UInt8(48), 120, 240, 255], count: 48 * 24).flatMap { $0 }
        bytes.withUnsafeBytes {
            texture.replace(region: MTLRegionMake2D(0, 0, 48, 24), mipmapLevel: 0,
                withBytes: $0.baseAddress!, bytesPerRow: 48 * 4)
        }
        let frame = Windowing.Frame(resourceID: 23, width: 48, height: 24,
            bytesPerRow: 48 * 4, format: .bgra8888, scale: 2, presentationID: 19)
        return (frame, texture)
    }

    @MainActor private func until(timeout: TimeInterval = 1.5, _ condition: () -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        while ProcessInfo.processInfo.systemUptime < deadline {
            if condition() { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        throw POSIXError(.ETIMEDOUT)
    }
}
