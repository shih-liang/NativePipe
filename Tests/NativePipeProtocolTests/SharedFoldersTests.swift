import Foundation
import XCTest
@testable import NativePipeProtocol

final class SharedFoldersTests: XCTestCase {
    func testSharedFolderMountRequestIsOneBooleanWithARequestID() {
        for mounted in [false, true] {
            let frame = ControlWire.encode(id: 42, call: .setSharedFoldersMounted(mounted))
            XCTAssertEqual(Array(frame), Array("NPSF".utf8)
                + [42, 0, 0, 0, 0, 0, 0, 0, mounted ? 1 : 0])
            XCTAssertTrue(ControlWire.isControlFrame(frame))
        }
    }
}
