import Foundation
import XCTest
@testable import NativePipeProtocol

@MainActor
final class AgentInputWireTests: XCTestCase {
    func testRequestLayoutMatchesCompositorReader() throws {
        let command = Windowing.HostCommand.agentInput(token: 9, window: 3, events: [
            .key(keycode: 30, pressed: true, modifiers: [.control]), .text("hé"),
        ])
        XCTAssertEqual(command.transportLane, .input)
        let payload = try WindowWire.commandPayload(for: command)
        var expected = Data("NPW2".utf8) + [2, 42, 0, 0]
        for value: UInt32 in [9, 3, 2] { withUnsafeBytes(of: value.littleEndian) { expected.append(contentsOf: $0) } }
        expected.append(1)
        withUnsafeBytes(of: UInt32(30).littleEndian) { expected.append(contentsOf: $0) }
        expected.append(1)
        withUnsafeBytes(of: Windowing.Modifiers.control.rawValue.littleEndian) { expected.append(contentsOf: $0) }
        expected.append(2)
        withUnsafeBytes(of: UInt32("hé".utf8.count).littleEndian) { expected.append(contentsOf: $0) }
        expected.append(contentsOf: "hé".utf8)
        XCTAssertEqual(payload, expected)

        let tooMuchText = String(repeating: "x", count: Windowing.AgentInputEvent.maximumTextBytes / 2 + 1)
        for invalid: Windowing.HostCommand in [
            .agentInput(token: 0, window: 3, events: [.text("a")]),
            .agentInput(token: 9, window: 3, events: []),
            .agentInput(token: 9, window: 3, events: [.text("")]),
            .agentInput(token: 9, window: 3, events: [.text("a\u{0}b")]),
            .agentInput(token: 9, window: 3, events: [.text(tooMuchText), .text(tooMuchText)]),
            .agentInput(token: 9, window: 3, events: Array(repeating: .key(keycode: 30, pressed: true, modifiers: []),
                                                          count: Windowing.AgentInputEvent.maximumEvents + 1)),
        ] {
            XCTAssertThrowsError(try WindowWire.commandPayload(for: invalid))
        }
    }

    func testReplyDecodesAsTheCompositorEncodesIt() throws {
        var data = Data("NPAP".utf8) + [1, 8, 0, 0]
        for value: UInt32 in [5, 1] { withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) } }
        let problem = "The user is typing in another window of this application."
        withUnsafeBytes(of: UInt32(problem.utf8.count).littleEndian) { data.append(contentsOf: $0) }
        data.append(contentsOf: problem.utf8)
        guard case .agentInput(5, 1, problem) = try ApplicationReply.decode(data) else {
            return XCTFail("Expected an agent input reply")
        }
        XCTAssertThrowsError(try ApplicationReply.decode(data + [0]))
    }

    func testClientReportsHowFarInputGot() async throws {
        var client: ApplicationClient!
        var sent: [[Windowing.AgentInputEvent]] = []
        client = ApplicationClient { command in
            guard case .agentInput(let token, 3, let events) = command else { return }
            sent.append(events)
            if events.count == 1 {
                client.receive(.agentInput(token, 0, "no text-input"))
            } else {
                client.receive(.agentInput(token, UInt32(events.count), ""))
            }
        }
        client.setConnected(true)
        let keys: [Windowing.AgentInputEvent] = [.key(keycode: 30, pressed: true, modifiers: []),
                                                 .key(keycode: 30, pressed: false, modifiers: [])]
        let receipt = try await client.agentInput(window: 3, events: keys)
        XCTAssertEqual(receipt, .init(accepted: 2, problem: nil))
        let rejected = try await client.agentInput(window: 3, events: [.text("你好")])
        XCTAssertEqual(rejected, .init(accepted: 0, problem: "no text-input"))
        XCTAssertEqual(sent, [keys, [.text("你好")]])
    }
}
