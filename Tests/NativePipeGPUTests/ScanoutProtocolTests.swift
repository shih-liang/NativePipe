import XCTest
@testable import NativePipeGPU

final class ScanoutProtocolTests: XCTestCase {
    func testMapInfoPreservesRendererCacheMode() throws {
        let data = VirtioGPU.MapInfoResponse(
            mapInfo: VirtioGPU.MapCache.writeCombine.rawValue).encoded()
        var reader = LittleEndianReader(data)
        XCTAssertEqual(
            try reader.readUInt32(), VirtioGPU.MapCache.writeCombine.rawValue)
        XCTAssertEqual(reader.remaining, 4)
    }

    func testVirgl3DCommandBodiesDecodeWireLayout() throws {
        var writer = LittleEndianWriter()
        for value: UInt32 in 1...11 { writer.write(value) }
        writer.pad(4)
        var reader = LittleEndianReader(writer.data)
        let create = try VirtioGPU.ResourceCreate3D(parsing: &reader)
        XCTAssertEqual(create.resourceID, 1)
        XCTAssertEqual(create.flags, 11)
        XCTAssertEqual(reader.remaining, 0)

        var transfer = LittleEndianWriter()
        for value: UInt32 in 1...6 { transfer.write(value) }
        transfer.write(UInt64(0x1122_3344_5566_7788))
        for value: UInt32 in 7...10 { transfer.write(value) }
        var transferReader = LittleEndianReader(transfer.data)
        let decoded = try VirtioGPU.Transfer3D(parsing: &transferReader)
        XCTAssertEqual(decoded.box.depth, 6)
        XCTAssertEqual(decoded.offset, 0x1122_3344_5566_7788)
        XCTAssertEqual(decoded.layerStride, 10)
        XCTAssertEqual(transferReader.remaining, 0)
    }

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
        XCTAssertEqual(reader.remaining, (VirtioGPU.maximumScanouts - 1) * 24)
    }

    func testRectRejectsOverflowAndOutOfBounds() {
        XCTAssertTrue(VirtioGPU.Rect(
            x: 10, y: 20, width: 100, height: 50).fits(width: 640, height: 480))
        XCTAssertFalse(VirtioGPU.Rect(
            x: 600, y: 20, width: 100, height: 50).fits(width: 640, height: 480))
        XCTAssertFalse(VirtioGPU.Rect(
            x: UInt32.max - 1, y: 0, width: 4, height: 1
        ).fits(width: UInt32.max, height: 1))
    }

    func testSetScanoutBlobWireLayout() throws {
        var writer = LittleEndianWriter()
        VirtioGPU.Rect(x: 4, y: 8, width: 640, height: 480).encoded(into: &writer)
        writer.write(UInt32(0))
        writer.write(UInt32(42))
        writer.write(UInt32(1024))
        writer.write(UInt32(768))
        writer.write(VirtioGPU.Format.b8g8r8a8Unorm.rawValue)
        writer.pad(4)
        writer.write(UInt32(4096))
        writer.write(UInt32(0))
        writer.write(UInt32(0))
        writer.write(UInt32(0))
        writer.write(UInt32(16384))
        writer.write(UInt32(0))
        writer.write(UInt32(0))
        writer.write(UInt32(0))

        var reader = LittleEndianReader(writer.data)
        let request = try VirtioGPU.SetScanoutBlob(parsing: &reader)
        XCTAssertEqual(request.resourceID, 42)
        XCTAssertEqual(request.rectangle.width, 640)
        XCTAssertEqual(request.width, 1024)
        XCTAssertEqual(request.strides[0], 4096)
        XCTAssertEqual(request.offsets[0], 16384)
        XCTAssertEqual(reader.remaining, 0)
    }
}
