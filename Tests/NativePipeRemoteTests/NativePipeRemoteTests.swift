import XCTest
@testable import NativePipeRemote

final class NativePipeRemoteTests: XCTestCase {
    func testSessionStartsDisconnectedAndDisconnectIsIdempotent() {
        let session = RemoteSession(host: "127.0.0.1", surfacePort: 1, mediaPort: 2)
        XCTAssertFalse(session.isConnected)
        session.disconnect()
        session.disconnect()
        XCTAssertFalse(session.isConnected)
    }
}
