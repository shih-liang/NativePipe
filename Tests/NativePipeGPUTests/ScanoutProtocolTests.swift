import XCTest
@testable import NativePipeGPU

final class ScanoutProtocolTests: XCTestCase {
    func testDisplayInfoHasExactlySixteenEntries() throws {
        let data = VirtioGPU.DisplayInfoResponse(
            width: 1280, height: 800, enabled: true).encoded()
        XCTAssertEqual(data.count, VirtioGPU.maximumScanouts * 24)
        var reader = LittleEndianReader(data)
        XCTAssertEqual(try reader.readUInt32(), 0)
        XCTAssertEqual(try reader.readUInt32(), 0)
        XCTAssertEqual(try reader.readUInt32(), 1280)
        XCTAssertEqual(try reader.readUInt32(), 800)
        XCTAssertEqual(try reader.readUInt32(), 1)
        XCTAssertEqual(try reader.readUInt32(), 0)
    }
}
