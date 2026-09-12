import XCTest
import IOSurface
import Metal
import NativePipeProtocol
@testable import NativePipeRemote

final class RemotePipelineTests: XCTestCase {
    @MainActor func testDecodeFailuresTerminateOnceAndCannotCrossReset() async throws {
        let frames = RemoteFrameSource()
        defer { frames.removeAll() }
        var failures: [String] = []
        frames.onFailure = { failures.append($0) }
        for (codec, payload) in [(MediaWire.Codec.alphaRLE, Data([128])), (.h264, Data([0, 0, 0, 1, 0x65, 0])), (.av1, Data([0x32, 1, 0]))] {
            let count = failures.count
            let header = MediaWire.Header(codec: codec, surfaceID: 1, resourceID: 1,
                width: 2, height: 2, ptsNanos: 0, payloadLength: UInt32(payload.count))
            XCTAssertTrue(frames.ingest(header: header, payload: payload))
            for _ in 0..<200 {
                if failures.count > count { break }
                try await Task.sleep(for: .milliseconds(5))
            }
            XCTAssertEqual(failures.count, count + 1, "Bad input must fail instead of leaving a scene waiting forever")
            XCTAssertFalse(frames.ingest(header: header, payload: payload))
            frames.removeAll()
            XCTAssertTrue(frames.ingest(header: header, payload: payload))
            frames.removeAll() // A queued old failure must not close the next connection.
            try await Task.sleep(for: .milliseconds(30))
            XCTAssertEqual(failures.count, count + 1)
        }
        XCTAssertFalse(frames.ingest(header: .init(surfaceID: 1, resourceID: 1,
            width: 2, height: 0, ptsNanos: 0, payloadLength: 1), payload: Data([0])))
    }

    @MainActor func testCreditsAndMediaProceedWhileMainActorIsBlockedAndStopRejectsOldInput() throws {
        let credit = DispatchSemaphore(value: 0), media = DispatchSemaphore(value: 0)
        let writer = WindowCommandWriter(remote: true) { _, data in
            if data.dropFirst(WireFormat.headerSize).prefix(4) == Data("NPRA".utf8) { credit.signal() }
        }
        let pipe = Pipe()
        writer.install(pipe.fileHandleForWriting)
        defer { writer.disconnect() }
        var delivered = 0
        let inbound = RemoteInbound(writer: writer, media: { _, _ in media.signal(); return true }) { _ in delivered += 1 }
        let header = MediaWire.Header(surfaceID: 1, resourceID: 1, width: 2, height: 2, ptsNanos: 0, payloadLength: 1)
        let reader = DispatchSemaphore(value: 0)
        DispatchQueue.global().async {
            inbound.receive(.event(.channelReady(sessionID: 1, protocolVersion: WindowWire.windowProtocolVersion)), bytes: 16)
            inbound.receive(.acknowledge(128), bytes: 0)
            inbound.receive(.media(header, Data([0])), bytes: 1)
            reader.signal()
        }
        // These must complete without a single main-actor hop.
        XCTAssertEqual(reader.wait(timeout: .now() + 2), .success)
        XCTAssertEqual(credit.wait(timeout: .now() + 2), .success)
        XCTAssertEqual(media.wait(timeout: .now() + 2), .success)
        XCTAssertEqual(delivered, 0)
        inbound.stop()
        XCTAssertFalse(inbound.receive(.media(header, Data([1])), bytes: 1))
        RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        XCTAssertEqual(delivered, 0, "Stopped generation must discard queued UI events")
        XCTAssertEqual(media.wait(timeout: .now()), .timedOut)
    }

    @MainActor func testOpaquePixelsSurviveCacheRetirementAndAlphaFramesUntilGPURead() async throws {
        try await checkPixels(codec: .h264)
    }

    @MainActor func testAV1PixelsSurviveCacheRetirementAndAlphaFramesUntilGPURead() async throws {
        try await checkPixels(codec: .av1)
    }

    @MainActor private func checkPixels(codec: MediaWire.Codec) async throws {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue(),
              let gate = device.makeSharedEvent() else { throw XCTSkip("Metal is unavailable") }
        let packets: [Data]
        if codec == .av1 { packets = try AV1DecoderTests.packets() }
        else {
            // Eight generated 128x96 testsrc2 all-I H.264 access units, with AUD.
            let url = try XCTUnwrap(Bundle.module.url(forResource: "lifetime", withExtension: "h264", subdirectory: "Resources"))
            let bytes = [UInt8](try Data(contentsOf: url))
            var starts: [Int] = [], i = 0
            while i + 4 < bytes.count {
                let size = bytes[i...].starts(with: [0,0,0,1]) ? 4 : bytes[i...].starts(with: [0,0,1]) ? 3 : 0
                if size > 0 { if bytes[i + size] & 31 == 9 { starts.append(i) }; i += size } else { i += 1 }
            }
            starts.append(bytes.count)
            XCTAssertEqual(starts.count, 9)
            packets = (0..<8).map { Data(bytes[starts[$0]..<starts[$0 + 1]]) }
        }
        let frames = RemoteFrameSource(), reference = RemoteFrameSource()
        defer { frames.removeAll(); reference.removeAll(); gate.signaledValue = 1 }
        let readback = try XCTUnwrap(device.makeBuffer(length: 128 * 96 * 4, options: .storageModeShared))
        var command: MTLCommandBuffer?
        var expected = Data()
        for index in 0..<8 {
            let resource = UInt32(index + 1)
            let payload = packets[index]
            var header = MediaWire.Header(codec: codec, surfaceID: 1, resourceID: resource, width: 128, height: 96,
                ptsNanos: UInt64(index), payloadLength: UInt32(payload.count))
            XCTAssertTrue(reference.ingest(header: header, payload: payload))
            if index > 0 {
                let alpha = Data(Array(repeating: [UInt8(129), 64], count: 96).flatMap { $0 })
                XCTAssertTrue(frames.ingest(header: .init(codec: .alphaRLE, surfaceID: 1, resourceID: resource,
                    width: 128, height: 96, ptsNanos: UInt64(index), payloadLength: UInt32(alpha.count)), payload: alpha))
                header.flags = MediaWire.flagHasAlpha
            }
            XCTAssertTrue(frames.ingest(header: header, payload: payload))
            for _ in 0..<400 {
                if frames.isResourcePublished(resource) && reference.isResourcePublished(resource) { break }
                try await Task.sleep(for: .milliseconds(5))
            }
            let resolution = frames.resolveFrame(forResource: resource, width: 128, height: 96, bytesPerRow: 512, format: 1)
            let surface = try XCTUnwrap(resolution.surface)
            if index == 0 {
                IOSurfaceLock(surface, .readOnly, nil)
                for row in 0..<96 { expected.append(IOSurfaceGetBaseAddress(surface).advanced(by: row * IOSurfaceGetBytesPerRow(surface)).assumingMemoryBound(to: UInt8.self), count: 512) }
                IOSurfaceUnlock(surface, .readOnly, nil)
                let texture = try XCTUnwrap(resolution.texture as? MTLTexture)
                let owner = try XCTUnwrap(resolution.owner)
                let gpu = try XCTUnwrap(queue.makeCommandBuffer())
                gpu.encodeWaitForEvent(gate, value: 1)
                let blit = try XCTUnwrap(gpu.makeBlitCommandEncoder())
                blit.copy(from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: .init(x: 0, y: 0, z: 0),
                    sourceSize: .init(width: 128, height: 96, depth: 1), to: readback, destinationOffset: 0,
                    destinationBytesPerRow: 512, destinationBytesPerImage: 512 * 96)
                blit.endEncoding()
                gpu.addCompletedHandler { _ in withExtendedLifetime(owner) {} }
                gpu.commit(); command = gpu
            } else {
                let original = try XCTUnwrap(reference.surface(forResource: resource))
                IOSurfaceLock(surface, .readOnly, nil)
                IOSurfaceLock(original, .readOnly, nil)
                var actual = Data(), expected = Data()
                for row in 0..<96 {
                    let source = IOSurfaceGetBaseAddress(original).advanced(by: row * IOSurfaceGetBytesPerRow(original)).assumingMemoryBound(to: UInt8.self)
                    for column in 0..<128 { expected.append(source.advanced(by: column * 4), count: 3); expected.append(64) }
                    actual.append(IOSurfaceGetBaseAddress(surface).advanced(by: row * IOSurfaceGetBytesPerRow(surface)).assumingMemoryBound(to: UInt8.self), count: 512)
                }
                IOSurfaceUnlock(original, .readOnly, nil)
                IOSurfaceUnlock(surface, .readOnly, nil)
                XCTAssertEqual(actual, expected, "Alpha injection must preserve every RGB component")
            }
        }
        XCTAssertFalse(frames.isResourcePublished(1), "Old cache entry should already be retired")
        XCTAssertTrue(frames.isResourcePublished(8), "Current static layer must stay resolvable")
        gate.signaledValue = 1
        let gpu = try XCTUnwrap(command)
        for _ in 0..<400 {
            if gpu.status == .completed || gpu.status == .error { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertEqual(gpu.status, .completed)
        XCTAssertEqual(Data(bytes: readback.contents(), count: 128 * 96 * 4), expected,
            "A queued GPU read must see the complete selected frame, including its original alpha")
    }
}
