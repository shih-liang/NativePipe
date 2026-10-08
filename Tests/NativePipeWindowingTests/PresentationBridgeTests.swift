import XCTest
import NativePipeProtocol
@testable import NativePipeWindowing

final class PresentationBridgeTests: XCTestCase {
    private func scene(session: UInt64 = 7, epoch: UInt64 = 1) -> Windowing.SceneSnapshot {
        .init(surface: 2, presentationID: 1, width: 64, height: 48, scale: 1,
              windowGeometry: .init(x: 0, y: 0, width: 64, height: 48), layers: [],
              presentationContext: .init(sessionID: session, clockEpoch: epoch, guestSendTimeNanoseconds: 100),
              receivedHostTimeNanoseconds: 123)
    }

    private func results(_ commands: [Windowing.HostCommand]) -> [(UInt64, UInt64, UInt32, UInt32, UInt64)] {
        commands.compactMap {
            if case .presentationFeedback(let session, let epoch, let surface, let id, let time, _, _) = $0 {
                return (session, epoch, surface, id, time)
            }
            return nil
        }
    }

    @MainActor func testUnacknowledgedDiscardReplaysAcrossSameCompositorReconnectOnly() {
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        bridge.apply(.sceneCommitted(scene: scene()))
        bridge.apply(.surfaceDestroyed(surface: 2))
        bridge.apply(.presentationClockRequested(token: 1, sessionID: 7, clockEpoch: 1))
        XCTAssertEqual(results(commands).map { $0.4 }, [0])
        XCTAssertTrue(commands.contains {
            if case .sceneClockSample(7, 1, 2, 1, 100, 123) = $0 { return true }
            return false
        }, "Use the reader's arrival clock instead of sampling after main-actor delivery")

        commands.removeAll()
        bridge.closeAll()
        bridge.apply(.channelReady(sessionID: 22, protocolVersion: WindowWire.windowProtocolVersion))
        XCTAssertTrue(results(commands).isEmpty, "A transport handshake does not establish the compositor nonce")
        bridge.apply(.presentationClockRequested(token: 2, sessionID: 7, clockEpoch: 2))
        XCTAssertEqual(results(commands).count, 1)
        XCTAssertEqual(results(commands).first?.0, 7)
        XCTAssertEqual(results(commands).first?.1, 1, "Replay keeps the original clock epoch")
        bridge.apply(.presentationFeedbackAcknowledged(sessionID: 7, clockEpoch: 1, surface: 2, presentationID: 1))

        commands.removeAll()
        bridge.closeAll()
        bridge.apply(.channelReady(sessionID: 23, protocolVersion: WindowWire.windowProtocolVersion))
        bridge.apply(.presentationClockRequested(token: 3, sessionID: 7, clockEpoch: 3))
        XCTAssertTrue(results(commands).isEmpty, "Only the guest acknowledgement retires a receipt")
        bridge.closeAll()
    }

    @MainActor func testNewCompositorCannotReceiveOldFeedbackForReusedSurfaceAndSceneIDs() {
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        bridge.apply(.sceneCommitted(scene: scene()))
        bridge.apply(.surfaceDestroyed(surface: 2))
        bridge.closeAll()
        commands.removeAll()
        bridge.apply(.channelReady(sessionID: 22, protocolVersion: WindowWire.windowProtocolVersion))
        bridge.apply(.presentationClockRequested(token: 2, sessionID: 8, clockEpoch: 1))
        XCTAssertTrue(results(commands).isEmpty)
        bridge.apply(.sceneCommitted(scene: scene(session: 8)))
        bridge.apply(.surfaceDestroyed(surface: 2))
        bridge.apply(.presentationClockRequested(token: 3, sessionID: 8, clockEpoch: 1))
        XCTAssertEqual(results(commands).map { $0.0 }, [8])
        bridge.closeAll()
    }

    @MainActor func testPauseDrainsDiscardedScenesAndResumeWaitsForMatchingClockReadyReceipt() async throws {
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        var resume: (UInt64, UInt32)?
        bridge.output = { command in
            commands.append(command)
            switch command {
            case .presentationPause(let session, let token):
                bridge.apply(.presentationPauseReached(sessionID: session, token: token))
            case .presentationDrain(let session, let token):
                XCTAssertFalse(self.results(commands).isEmpty, "The drain must follow terminal feedback")
                bridge.apply(.presentationDrained(sessionID: session, token: token))
            case .presentationResume(let session, let token): resume = (session, token)
            default: break
            }
        }
        defer { bridge.output = nil; bridge.closeAll() }
        bridge.apply(.sceneCommitted(scene: scene()))
        try await bridge.prepareForVirtualMachinePause()
        XCTAssertFalse(bridge.acceptsKeyboardInput)
        XCTAssertEqual(results(commands).last?.4, 0, "A queued scene is proven never submitted")
        let pauseIndex = try XCTUnwrap(commands.firstIndex {
            if case .presentationPause = $0 { return true }; return false
        })
        let drainIndex = try XCTUnwrap(commands.firstIndex {
            if case .presentationDrain = $0 { return true }; return false
        })
        XCTAssertLessThan(pauseIndex, drainIndex)

        let task = Task { @MainActor in try await bridge.resumePresentationAfterVirtualMachinePause() }
        for _ in 0..<100 {
            if resume != nil { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        let (session, token) = try XCTUnwrap(resume)
        XCTAssertFalse(bridge.acceptsKeyboardInput)
        bridge.apply(.presentationResumed(sessionID: session + 1, clockEpoch: 2, token: token))
        bridge.apply(.presentationResumed(sessionID: session, clockEpoch: 2, token: token + 1))
        XCTAssertFalse(bridge.acceptsKeyboardInput)
        bridge.apply(.presentationResumed(sessionID: session, clockEpoch: 2, token: token))
        try await task.value
        XCTAssertTrue(bridge.acceptsKeyboardInput)
    }

    @MainActor func testEmptyCompositorPauseAndResumeRequireNoTransport() async throws {
        let bridge = WindowBridge(frameSource: nil)
        defer { bridge.closeAll() }
        try await bridge.prepareForVirtualMachinePause()
        XCTAssertFalse(bridge.acceptsKeyboardInput)
        try await bridge.resumePresentationAfterVirtualMachinePause()
        XCTAssertTrue(bridge.acceptsKeyboardInput)
    }

    @MainActor func testPauseCannotDrainBeforeLateControlLatchesCrossTheGuestFence() async throws {
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        var firstPause: (UInt64, UInt32)?
        var controlFence: (UInt64, UInt32)?
        bridge.output = { command in
            commands.append(command)
            switch command {
            case .presentationPause(let session, let token):
                if firstPause == nil {
                    firstPause = (session, token)
                    bridge.apply(.presentationPauseReached(sessionID: session, token: token))
                    // Model a worker's latch arriving after the initial display
                    // receipt. The control writer and feedback writer can run
                    // in either order until the second receipt is observed.
                    bridge.send(.framePresented(surface: 2, presentationID: 9))
                } else { controlFence = (session, token) }
            case .presentationDrain(let session, let token):
                XCTAssertEqual(token, controlFence?.1)
                bridge.apply(.presentationDrained(sessionID: session, token: token))
            default: break
            }
        }
        defer { bridge.output = nil; bridge.closeAll() }
        bridge.apply(.sceneCommitted(scene: scene()))
        let task = Task { @MainActor in try await bridge.prepareForVirtualMachinePause() }
        for _ in 0..<100 {
            if controlFence != nil { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        let (session, token) = try XCTUnwrap(controlFence)
        XCTAssertNotEqual(token, firstPause?.1)
        XCTAssertFalse(commands.contains { if case .presentationDrain = $0 { return true }; return false })
        bridge.apply(.presentationPauseReached(sessionID: session, token: try XCTUnwrap(firstPause).1))
        try await Task.sleep(for: .milliseconds(15))
        XCTAssertFalse(commands.contains { if case .presentationDrain = $0 { return true }; return false },
                       "The old receipt cannot fence a newly flushed control command")
        bridge.apply(.presentationPauseReached(sessionID: session, token: token))
        try await task.value
        let latchIndex = try XCTUnwrap(commands.firstIndex {
            if case .framePresented(_, 9) = $0 { return true }; return false
        })
        let controlFenceIndex = try XCTUnwrap(commands.firstIndex {
            if case .presentationPause(_, token) = $0 { return true }; return false
        })
        let drainIndex = try XCTUnwrap(commands.firstIndex {
            if case .presentationDrain = $0 { return true }; return false
        })
        XCTAssertLessThan(latchIndex, controlFenceIndex)
        XCTAssertLessThan(controlFenceIndex, drainIndex)
    }

    @MainActor func testDetachDuringPauseReportsTheConnectionFailureAndPreservesReceipts() async throws {
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { command in
            commands.append(command)
            if case .presentationPause = command { bridge.closeAll() }
        }
        defer { bridge.output = nil; bridge.closeAll() }
        bridge.apply(.sceneCommitted(scene: scene()))
        do {
            try await bridge.prepareForVirtualMachinePause()
            XCTFail("A changed transport cannot establish the pause barrier")
        } catch WindowPresentationPauseError.connectionChanged {} catch { XCTFail("Unexpected error: \(error)") }
        XCTAssertTrue(bridge.acceptsKeyboardInput, "The still-running VM must be rolled back")
        commands.removeAll()
        bridge.apply(.channelReady(sessionID: 22, protocolVersion: WindowWire.windowProtocolVersion))
        bridge.apply(.presentationClockRequested(token: 2, sessionID: 7, clockEpoch: 2))
        XCTAssertEqual(results(commands).map { $0.4 }, [0])
    }

    @MainActor func testCancelledPauseCalibratesRollbackAndSkipsOnlyTheDuplicateControllerHook() async throws {
        let bridge = WindowBridge(frameSource: nil)
        var pauseRequested = false
        var resumeCount = 0
        bridge.output = { command in
            switch command {
            case .presentationPause: pauseRequested = true
            case .presentationResume(let session, let token):
                resumeCount += 1
                bridge.apply(.presentationResumed(sessionID: session,
                    clockEpoch: UInt64(resumeCount + 1), token: token))
            default: break
            }
        }
        defer { bridge.output = nil; bridge.closeAll() }
        bridge.apply(.presentationClockRequested(token: 1, sessionID: 7, clockEpoch: 1))
        let task = Task { @MainActor in try await bridge.prepareForVirtualMachinePause() }
        for _ in 0..<100 {
            if pauseRequested { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(pauseRequested)
        task.cancel()
        do {
            try await task.value
            XCTFail("The original cancellation must still reach the caller")
        } catch is CancellationError {} catch { XCTFail("Unexpected error: \(error)") }
        XCTAssertEqual(resumeCount, 1, "Cancellation cannot abort the separate rollback handshake")
        XCTAssertTrue(bridge.acceptsKeyboardInput)

        // VMController's catch invokes the same hook after prepare rolls back.
        try await bridge.resumePresentationAfterVirtualMachinePause()
        XCTAssertEqual(resumeCount, 1)
        // A real VZ pause/restore cannot reuse that calibration.
        bridge.setSuspended(true)
        try await bridge.resumePresentationAfterVirtualMachinePause()
        XCTAssertEqual(resumeCount, 2)
        XCTAssertTrue(bridge.acceptsKeyboardInput)

        bridge.closeAll()
        bridge.apply(.channelReady(sessionID: 22, protocolVersion: WindowWire.windowProtocolVersion))
        bridge.apply(.presentationClockRequested(token: 2, sessionID: 8, clockEpoch: 1))
        try await bridge.resumePresentationAfterVirtualMachinePause()
        XCTAssertEqual(resumeCount, 3, "A new compositor requires its own calibration")
    }
}
