import XCTest
import QuartzCore
@testable import NativePipeProtocol

final class PresentationWireTests: XCTestCase {
    private func append<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var value = value.littleEndian
        withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
    }
    private func event(_ opcode: UInt8) -> Data { Data([78, 80, 87, 50, 1, opcode, 0, 0]) }

    private func auxiliaryFrame(withOptions: Bool = false, requestsFeedback: Bool = false) -> Data {
        var data = event(21)
        append(UInt32(17), to: &data)
        append(UInt32(41), to: &data)
        for field: Int32 in [32, 24, 128, 2] { append(field, to: &data) }
        data.append(contentsOf: [1, withOptions ? 3 : 2])
        append(UInt16(7), to: &data)
        append(UInt32(11), to: &data)
        append(UInt32(withOptions ? 0x3f : 0x20) | (requestsFeedback ? 0x40 : 0), to: &data)
        append(UInt32(withOptions ? 1 : 0), to: &data)
        if withOptions {
            for number: Double in [2, 3, 20, 16] { append(number.bitPattern, to: &data) }
            for field: Int32 in [10, 8, 1, 2, 16, 12] { append(field, to: &data) }
            append(UInt32(4), to: &data); data.append(contentsOf: "h264".utf8)
            append(UInt32(55), to: &data)
            for field: Int32 in [0, 0, 8, 8] { append(field, to: &data) }
        }
        for field: UInt64 in [42, 3, 1_234_567_890] { append(field, to: &data) }
        return data
    }

    func testAuxiliaryFrameHasExactClockIdentityAndHostReceiptSample() throws {
        let before = UInt64(CACurrentMediaTime() * 1_000_000_000)
        guard case .committed(let surface, let frame) = try WindowWire.guestEvent(from: auxiliaryFrame())
        else { return XCTFail("auxiliary frame lost") }
        let after = UInt64(CACurrentMediaTime() * 1_000_000_000)
        XCTAssertEqual(surface, 17)
        XCTAssertEqual(frame.resourceID, 41)
        XCTAssertEqual(frame.presentationID, 11)
        XCTAssertFalse(frame.requestsPresentationFeedback)
        XCTAssertEqual(frame.presentationContext, .init(
            sessionID: 42, clockEpoch: 3, guestSendTimeNanoseconds: 1_234_567_890))
        let received = try XCTUnwrap(frame.receivedHostTimeNanoseconds)
        XCTAssertGreaterThanOrEqual(received, before)
        XCTAssertLessThanOrEqual(received, after)
    }

    func testAuxiliaryClockTrailerFollowsViewportCodecAndDamage() throws {
        guard case .committed(_, let frame) = try WindowWire.guestEvent(from: auxiliaryFrame(withOptions: true, requestsFeedback: true))
        else { return XCTFail("auxiliary metadata lost") }
        XCTAssertEqual(frame.viewportSource, .init(x: 2, y: 3, width: 20, height: 16))
        XCTAssertEqual(frame.viewportDestination, .init(width: 10, height: 8))
        XCTAssertEqual(frame.windowGeometry, .init(x: 1, y: 2, width: 16, height: 12))
        XCTAssertEqual(frame.codec, "h264")
        XCTAssertEqual(frame.damage, [.init(x: 0, y: 0, width: 8, height: 8)])
        XCTAssertEqual(frame.presentationContext?.guestSendTimeNanoseconds, 1_234_567_890)
        XCTAssertTrue(frame.requestsPresentationFeedback)
    }

    func testAuxiliaryFrameRejectsMissingMalformedAndTruncatedClockIdentity() {
        let frame = auxiliaryFrame()
        XCTAssertEqual(frame.count, 72)
        for count in 0..<frame.count {
            XCTAssertThrowsError(try WindowWire.guestEvent(from: Data(frame.prefix(count))))
        }
        XCTAssertThrowsError(try WindowWire.guestEvent(from: frame + Data([0])))
        for range in [8..<12, 36..<40, 48..<56, 56..<64, 64..<72] {
            var malformed = frame
            malformed.replaceSubrange(range, with: Array(repeating: UInt8(0), count: range.count))
            XCTAssertThrowsError(try WindowWire.guestEvent(from: malformed))
        }
        for flags: UInt8 in [0, 0x40, 0x80, 0xa0] {
            var malformed = frame; malformed[40] = flags
            XCTAssertThrowsError(try WindowWire.guestEvent(from: malformed))
        }
    }

    func testCalibrationCarriesCompositorIdentityAndEpoch() throws {
        var request = event(44)
        append(UInt32(9), to: &request)
        append(UInt64(42), to: &request)
        append(UInt64(3), to: &request)
        guard case .presentationClockRequested(let token, let session, let epoch) =
                try WindowWire.guestEvent(from: request) else { return XCTFail("clock calibration request lost") }
        XCTAssertEqual(token, 9); XCTAssertEqual(session, 42); XCTAssertEqual(epoch, 3)
        XCTAssertThrowsError(try WindowWire.guestEvent(from: request + Data([0])))
        XCTAssertThrowsError(try WindowWire.guestEvent(from: Data(request.dropLast())))
        for offset in [8, 12, 20] {
            var malformed = request; malformed[offset] = 0
            XCTAssertThrowsError(try WindowWire.guestEvent(from: malformed))
        }
    }

    func testActualTimestampUsesASeparateCommandFromLatchAndRelease() throws {
        let time: UInt64 = 0x1122334455667788
        let actual = Windowing.HostCommand.presentationFeedback(sessionID: 42, clockEpoch: 3,
            surface: 3, presentationID: 7, hostTimeNanoseconds: time,
            refreshNanoseconds: 16_666_667, outputID: 4)
        XCTAssertEqual(actual.transportLane, .feedback)
        let payload = try WindowWire.commandPayload(for: actual)
        XCTAssertEqual(payload.count, 48)
        XCTAssertEqual(Array(payload.prefix(8)), [78, 80, 87, 50, 2, 36, 0, 0])
        XCTAssertEqual(Array(payload[32..<40]), [0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11])
        XCTAssertNil(WindowWire.frameTimingPayload(for: [actual][...]))
        let discarded = try WindowWire.commandPayload(for: .presentationFeedback(
            sessionID: 42, clockEpoch: 3, surface: 3, presentationID: 7,
            hostTimeNanoseconds: 0, refreshNanoseconds: 0, outputID: 0))
        XCTAssertEqual(Array(discarded[32..<40]), Array(repeating: 0, count: 8))
        XCTAssertThrowsError(try WindowWire.commandPayload(for: .presentationFeedback(
            sessionID: 0, clockEpoch: 3, surface: 3, presentationID: 7,
            hostTimeNanoseconds: time, refreshNanoseconds: 0, outputID: 0)))
    }

    func testClockSamplePreservesNanosecondsAndRejectsZeroIdentity() throws {
        let sample = Windowing.HostCommand.presentationClockSample(token: 9, sessionID: 42,
            clockEpoch: 3, hostTimeNanoseconds: 1_234_567_890)
        XCTAssertEqual(sample.transportLane, .feedback)
        let payload = try WindowWire.commandPayload(for: sample)
        XCTAssertEqual(payload.count, 36)
        XCTAssertEqual(payload[5], 35)
        XCTAssertEqual(Array(payload[28..<36]), [0xd2, 0x02, 0x96, 0x49, 0, 0, 0, 0])
        for invalid in [
            Windowing.HostCommand.presentationClockSample(token: 0, sessionID: 42, clockEpoch: 3, hostTimeNanoseconds: 1),
            .presentationClockSample(token: 1, sessionID: 0, clockEpoch: 3, hostTimeNanoseconds: 1),
            .presentationClockSample(token: 1, sessionID: 42, clockEpoch: 0, hostTimeNanoseconds: 1),
            .presentationClockSample(token: 1, sessionID: 42, clockEpoch: 3, hostTimeNanoseconds: 0)
        ] { XCTAssertThrowsError(try WindowWire.commandPayload(for: invalid)) }
    }

    func testSceneAnchorAndDrainShareTheResultLane() throws {
        let anchor = Windowing.HostCommand.sceneClockSample(sessionID: 42, clockEpoch: 3,
            surface: 4, presentationID: 5, guestSendTimeNanoseconds: 6, hostReceiveTimeNanoseconds: 7)
        XCTAssertEqual(anchor.transportLane, .feedback)
        let payload = try WindowWire.commandPayload(for: anchor)
        XCTAssertEqual(payload[5], 37); XCTAssertEqual(payload.count, 48)
        for (command, opcode, lane) in [
            (Windowing.HostCommand.presentationPause(sessionID: 42, token: 9), UInt8(38), WindowTransportLane.control),
            (.presentationDrain(sessionID: 42, token: 9), 39, .feedback),
            (.presentationResume(sessionID: 42, token: 9), 40, .control)
        ] {
            XCTAssertEqual(command.transportLane, lane)
            let data = try WindowWire.commandPayload(for: command)
            XCTAssertEqual(data[5], opcode); XCTAssertEqual(data.count, 20)
            XCTAssertNil(WindowWire.frameTimingPayload(for: [command][...]))
        }
    }

    func testResultAcknowledgementAndPauseFencesAreExactAndSessionScoped() throws {
        var ack = event(47)
        append(UInt64(42), to: &ack); append(UInt64(3), to: &ack)
        append(UInt32(4), to: &ack); append(UInt32(5), to: &ack)
        guard case .presentationFeedbackAcknowledged(let session, let epoch, let surface, let scene) =
                try WindowWire.guestEvent(from: ack) else { return XCTFail("missing result acknowledgement") }
        XCTAssertEqual(session, 42); XCTAssertEqual(epoch, 3)
        XCTAssertEqual(surface, 4); XCTAssertEqual(scene, 5)
        XCTAssertThrowsError(try WindowWire.guestEvent(from: ack + Data([0])))
        for opcode: UInt8 in [48, 49, 50] {
            var fence = event(opcode)
            append(UInt64(42), to: &fence)
            if opcode == 50 { append(UInt64(4), to: &fence) }
            append(UInt32(9), to: &fence)
            let decoded = try WindowWire.guestEvent(from: fence)
            switch decoded {
            case .presentationPauseReached(let session, let token), .presentationDrained(let session, let token):
                XCTAssertEqual(session, 42); XCTAssertEqual(token, 9)
            case .presentationResumed(let session, let epoch, let token):
                XCTAssertEqual(session, 42); XCTAssertEqual(epoch, 4); XCTAssertEqual(token, 9)
            default: XCTFail("missing fence")
            }
            XCTAssertThrowsError(try WindowWire.guestEvent(from: Data(fence.dropLast())))
            XCTAssertThrowsError(try WindowWire.guestEvent(from: fence + Data([0])))
        }
    }
}
