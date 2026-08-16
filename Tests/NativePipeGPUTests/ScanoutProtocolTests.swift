import IOSurface
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

    func testCreate2DUsesAlignedIOSurfaceWithoutChangingGuestStride() throws {
        let resource = try ResourceTable().create2D(
            id: 7,
            format: VirtioGPU.Format.b8g8r8x8Unorm.rawValue,
            width: 1279,
            height: 17)
        let surface = try XCTUnwrap(resource.surface)
        let metadata = try XCTUnwrap(resource.twoDimensional)
        XCTAssertEqual(metadata.sourceBytesPerRow, 1279 * 4)
        XCTAssertGreaterThanOrEqual(IOSurfaceGetBytesPerRow(surface), metadata.sourceBytesPerRow)
    }
}
