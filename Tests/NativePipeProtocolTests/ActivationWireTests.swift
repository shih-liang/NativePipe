import XCTest
@testable import NativePipeProtocol

final class ActivationWireTests: XCTestCase {
    private func event(window: UInt32 = 12, origin: UInt32 = 8, age: UInt32 = 120) -> Data {
        var payload = Data("NPW2".utf8)
        payload.append(contentsOf: [1, 43, 0, 0])
        for field in [window, origin, age] {
            var value = field.littleEndian
            withUnsafeBytes(of: &value) { payload.append(contentsOf: $0) }
        }
        return payload
    }

    func testActivationPreservesOriginAndInputAge() throws {
        guard case .activationRequested(let window, let origin, let age) =
            try WindowWire.guestEvent(from: event()) else { return XCTFail("Missing activation request") }
        XCTAssertEqual(window, 12)
        XCTAssertEqual(origin, 8)
        XCTAssertEqual(age, 120)
        XCTAssertNoThrow(try WindowWire.guestEvent(from: event(age: 5_000)))
    }

    func testMalformedOrExpiredAuthorityIsRejected() {
        for payload in [event(window: 0), event(origin: 0), event(age: 5_001),
                        event(age: .max), Data(event().dropLast()), event() + Data([0])] {
            XCTAssertThrowsError(try WindowWire.guestEvent(from: payload))
        }
    }
}
