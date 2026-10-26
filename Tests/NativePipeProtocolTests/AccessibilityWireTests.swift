import Foundation
import XCTest
@testable import NativePipeProtocol

@MainActor
final class AccessibilityWireTests: XCTestCase {
    func testRequestLayoutMatchesCompositorReader() throws {
        let request = GuestAccessibilityRequest(window: 7, operation: .setText, number: 2.5,
                                           element: ":1.4/a", argument: "hé", fullText: true)
        let payload = try WindowWire.commandPayload(for: .accessibilityRequest(token: 9, request: request))
        var expected = Data("NPW2".utf8) + [2, 41, 0, 0]
        for value: UInt32 in [9, 7, 4, 2] { withUnsafeBytes(of: value.littleEndian) { expected.append(contentsOf: $0) } }
        withUnsafeBytes(of: (2.5).bitPattern.littleEndian) { expected.append(contentsOf: $0) }
        for text in [":1.4/a", "hé"] {
            withUnsafeBytes(of: UInt32(text.utf8.count).littleEndian) { expected.append(contentsOf: $0) }
            expected.append(contentsOf: text.utf8)
        }
        XCTAssertEqual(payload, expected)

        var oversized = request
        oversized.argument = String(repeating: "x", count: GuestAccessibilityRequest.maximumArgumentSize + 1)
        XCTAssertThrowsError(try WindowWire.commandPayload(for: .accessibilityRequest(token: 9, request: oversized)))
        oversized = request
        oversized.number = .nan
        XCTAssertThrowsError(try WindowWire.commandPayload(for: .accessibilityRequest(token: 9, request: oversized)))
    }

    func testReplyDecodesAsTheCompositorEncodesIt() throws {
        func reply(token: UInt32, error: String, json: String) -> Data {
            var data = Data("NPAP".utf8) + [1, 7, 0, 0]
            withUnsafeBytes(of: token.littleEndian) { data.append(contentsOf: $0) }
            for text in [error, json] {
                withUnsafeBytes(of: UInt32(text.utf8.count).littleEndian) { data.append(contentsOf: $0) }
                data.append(contentsOf: text.utf8)
            }
            return data
        }
        guard case .accessibility(3, "", let json) = try ApplicationReply.decode(reply(token: 3, error: "", json: "{}")) else {
            return XCTFail("Expected an accessibility reply")
        }
        XCTAssertEqual(json, Data("{}".utf8))
        // Token zero is reserved for unsolicited notices.
        XCTAssertThrowsError(try ApplicationReply.decode(reply(token: 0, error: "", json: "{}")))
    }

    func testClientRoundTripAndErrors() async throws {
        let document = """
        {"application":"gedit","toolkit":"GTK 3.24.41","pid":42,"window":":1.4/w","root":\
        {"ref":":1.4/w","role":"frame","name":"Notes","states":["active","showing"],"children":\
        [{"ref":":1.4/b","role":"push button","name":"Save","states":["showing"],"bounds":[15,24,80,24],\
        "actions":["click"]}]},"truncated":false}
        """
        var client: ApplicationClient!
        var seen: [GuestAccessibilityRequest] = []
        client = ApplicationClient { command in
            guard case .accessibilityRequest(let token, let request) = command else { return }
            seen.append(request)
            if request.operation == .action {
                client.receive(.accessibility(token, "The element has no actions.", Data()))
            } else {
                client.receive(.accessibility(token, "", Data(document.utf8)))
            }
        }
        client.setConnected(true)
        let tree = try await client.accessibilityTree(.init(window: 3, operation: .tree, number: 6))
        XCTAssertEqual(tree.application, "gedit")
        XCTAssertEqual(tree.root?.children?.first?.bounds, [15, 24, 80, 24])
        XCTAssertEqual(tree.root?.children?.first?.actions, ["click"])
        do {
            _ = try await client.accessibility(.init(window: 3, operation: .action, element: ":1.4/w"))
            XCTFail("A worker error must fail the request")
        } catch let failure as ApplicationClient.Failure {
            XCTAssertEqual(failure.message, "The element has no actions.")
        }
        XCTAssertEqual(seen.map(\.operation), [.tree, .action])
        // An application reply for an accessibility token is ignored.
        client.receive(.launched(1, 0, ""))
    }
}
