import Foundation
import XCTest
@testable import NativePipeProtocol

final class GuestNotificationHistoryTests: XCTestCase {
    func testDeliveryStatesRoundTripWithRecordIdentityAndTimestamp() throws {
        for delivery in [GuestNotificationDelivery.pending, .delivered, .notAuthorized, .failed, .cancelled] {
            let record = GuestNotificationHistoryRecord(id: "nativepipe.guest.session.1.2", machine: "开发机",
                application: "Terminal", title: "Build finished", body: "Line one\nLine two",
                receivedAt: Date(timeIntervalSince1970: 1234.5), delivery: delivery)
            let data = try JSONEncoder().encode(record)
            XCTAssertEqual(try JSONDecoder().decode(GuestNotificationHistoryRecord.self, from: data), record)
        }
    }
}
