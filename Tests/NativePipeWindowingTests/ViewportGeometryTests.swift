import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class ViewportGeometryTests: XCTestCase {
    func testFirefoxBufferMapsToViewportDestinationWithoutChangingBufferScale() {
        let frame = Windowing.Frame(
            resourceID: 47, width: 1_600, height: 1_200,
            bytesPerRow: 6_400, format: .bgra8888, scale: 1,
            viewportDestination: .init(width: 800, height: 600))

        XCTAssertEqual(frame.logicalSurfaceSize, CGSize(width: 800, height: 600))
        XCTAssertEqual(frame.appKitPointSize, CGSize(width: 800, height: 600))
        XCTAssertEqual(
            frame.fullViewportBufferPixelRect,
            CGRect(x: 0, y: 0, width: 1_600, height: 1_200))
        XCTAssertEqual(
            frame.pixelDensity(for: CGRect(x: 0, y: 0, width: 800, height: 600)),
            2)
    }

    func testViewportCropMapsLogicalGeometryBackToBufferPixels() {
        let frame = Windowing.Frame(
            resourceID: 7, width: 100, height: 80,
            bytesPerRow: 400, format: .bgra8888, scale: 2,
            viewportSource: .init(x: 5, y: 3, width: 40, height: 30),
            viewportDestination: .init(width: 20, height: 15))

        XCTAssertEqual(
            frame.bufferPixelRect(for: CGRect(x: 2, y: 1, width: 10, height: 5)),
            CGRect(x: 18, y: 10, width: 40, height: 20))
    }

    func testPendingCursorIsNotVisibleBeforeCoalescedInstallation() {
        let bridge = WindowBridge(frameSource: nil)
        bridge.apply(.cursorChanged(surface: nil, hotspotX: 0, hotspotY: 0))

        XCTAssertTrue(bridge.currentPointerCursor() === NSCursor.arrow)
        bridge.closeAll()
    }

    func testNativeCursorShapesUseAppKitCursorObjects() {
        XCTAssertTrue(
            NativeCursorResolver.cursor(for: .defaultShape) === NSCursor.arrow)
        XCTAssertTrue(
            NativeCursorResolver.cursor(for: .pointer) === NSCursor.pointingHand)
        XCTAssertTrue(
            NativeCursorResolver.cursor(for: .text) === NSCursor.iBeam)
        XCTAssertTrue(
            NativeCursorResolver.cursor(for: .copy) === NSCursor.dragCopy)
        XCTAssertTrue(
            NativeCursorResolver.cursor(for: .notAllowed) === NSCursor.operationNotAllowed)
    }

    func testCustomCursorSurfaceUsesTheSamePointGeometryAsAWindowSurface() {
        let frame = Windowing.Frame(
            resourceID: 12, width: 64, height: 64, bytesPerRow: 256,
            format: .bgra8888, scale: 2,
            viewportSource: .init(x: 4, y: 2, width: 24, height: 20),
            viewportDestination: .init(width: 18, height: 15))

        XCTAssertEqual(frame.appKitPointSize, CGSize(width: 18, height: 15))
        XCTAssertEqual(
            frame.fullViewportBufferPixelRect,
            CGRect(x: 8, y: 4, width: 48, height: 40))
    }

    func testTwoXCustomCursorIsNotInstalledAtPixelSize() {
        let frame = Windowing.Frame(
            resourceID: 13, width: 64, height: 64, bytesPerRow: 256,
            format: .bgra8888, scale: 2)

        let geometry = WindowBridge.customCursorGeometry(
            frame: frame, hotSpot: CGPoint(x: 6, y: 8))

        XCTAssertEqual(geometry.imageSize, CGSize(width: 32, height: 32))
        XCTAssertEqual(
            geometry.sourcePixels, CGRect(x: 0, y: 0, width: 64, height: 64))
        XCTAssertEqual(geometry.hotSpot, CGPoint(x: 6, y: 8))
    }

    func testCustomCursorViewportAndHotspotUseLogicalCoordinates() {
        let frame = Windowing.Frame(
            resourceID: 14, width: 96, height: 80, bytesPerRow: 384,
            format: .bgra8888, scale: 2,
            viewportSource: .init(x: 4, y: 3, width: 32, height: 24),
            viewportDestination: .init(width: 20, height: 15))

        let geometry = WindowBridge.customCursorGeometry(
            frame: frame, hotSpot: CGPoint(x: 99, y: -3))

        XCTAssertEqual(geometry.imageSize, CGSize(width: 20, height: 15))
        XCTAssertEqual(
            geometry.sourcePixels, CGRect(x: 8, y: 6, width: 64, height: 48))
        XCTAssertEqual(geometry.hotSpot, CGPoint(x: 19, y: 0))
    }

    func testDroppedCPUFrameIsAcknowledgedSoGuestBlobCannotDeadlock() {
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        bridge.apply(.surfaceCreated(surface: 8))
        bridge.apply(.toplevelCreated(window: 3, surface: 8))
        bridge.apply(.committed(
            surface: 8,
            frame: Windowing.Frame(
                resourceID: 99, width: 64, height: 48, bytesPerRow: 256,
                format: .bgra8888, presentationID: 17)))

        guard let command = commands.last,
              case .framePresented(let surface, let presentationID) = command
        else {
            return XCTFail("dropped CPU frame was not acknowledged")
        }
        XCTAssertEqual(surface, 8)
        XCTAssertEqual(presentationID, 17)
        bridge.closeAll()
    }

    func testEmptyDamageDoesNotUpdateWindow() {
        XCTAssertEqual(
            MetalPresenter.clippedDamageRects([], width: 640, height: 480),
            [])
    }

    func testGPUEmptyDamageDoesNotUpdateWindow() {
        XCTAssertEqual(
            MetalPresenter.clippedDamageRects([], width: 640, height: 480),
            [])
    }

    func testFullFrameDamageUsesExplicitRect() {
        XCTAssertEqual(
            MetalPresenter.clippedDamageRects(
                [.init(x: 0, y: 0, width: 640, height: 480)],
                width: 640, height: 480),
            [.init(x: 0, y: 0, width: 640, height: 480)])
    }

    func testResizeOverlapKeepsTopLeftHistory() {
        XCTAssertEqual(
            MetalPresenter.overlapCopyRect(
                oldWidth: 800, oldHeight: 600, newWidth: 1024, newHeight: 768),
            .init(x: 0, y: 0, width: 800, height: 600))
        XCTAssertEqual(
            MetalPresenter.overlapCopyRect(
                oldWidth: 1024, oldHeight: 768, newWidth: 800, newHeight: 600),
            .init(x: 0, y: 0, width: 800, height: 600))
        XCTAssertNil(
            MetalPresenter.overlapCopyRect(
                oldWidth: 0, oldHeight: 600, newWidth: 800, newHeight: 600))
    }

    func testGPUSubsurfaceCommitIsDeferredInsteadOfDropped() {
        let bridge = WindowBridge(frameSource: nil)
        var presented: [(UInt32, UInt32)] = []
        bridge.output = { command in
            if case .framePresented(let surface, let presentationID) = command {
                presented.append((surface, presentationID))
            }
        }
        bridge.apply(.surfaceCreated(surface: 1))
        bridge.apply(.surfaceCreated(surface: 2))
        bridge.apply(.toplevelCreated(window: 3, surface: 1))
        bridge.apply(.subsurfaceCreated(surface: 2, parent: 1, x: 10, y: 20))
        bridge.apply(.committed(
            surface: 2,
            frame: Windowing.Frame(
                resourceID: 99, width: 64, height: 48, bytesPerRow: 256,
                format: .bgra8888,
                damage: [.init(x: 0, y: 0, width: 64, height: 48)],
                source: .gpu, presentationID: 7)))

        XCTAssertFalse(
            presented.contains { $0 == (2, 7) },
            "a GPU subsurface without a host mapping must wait, not be released")
        bridge.closeAll()
    }

    func testGPUDamageIsClippedBeforeMetalCopies() {
        XCTAssertEqual(
            MetalPresenter.clippedDamageRects(
                [
                    .init(x: -5, y: 7, width: 13, height: 9),
                    .init(x: 95, y: 78, width: Int.max, height: Int.max),
                    .init(x: 3, y: 4, width: 0, height: 8),
                    .init(x: 200, y: 4, width: 5, height: 8),
                ],
                width: 100, height: 80),
            [
                .init(x: 0, y: 7, width: 8, height: 9),
                .init(x: 95, y: 78, width: 5, height: 2),
            ])
        XCTAssertEqual(
            MetalPresenter.clippedDamageRects(
                [], width: 100, height: 80, emptyMeansFull: false),
            [])
    }

    func testMetalCopyRectsAreClippedToBothTextures() {
        let clipped = MetalPresenter.clipCopyRect(
            sourceX: 10, sourceY: -4, destX: 0, destY: 0,
            width: 80, height: 40,
            sourceWidth: 64, sourceHeight: 32,
            destWidth: 50, destHeight: 20)
        XCTAssertEqual(clipped?.sourceX, 10)
        XCTAssertEqual(clipped?.sourceY, 0)
        XCTAssertEqual(clipped?.destX, 0)
        XCTAssertEqual(clipped?.destY, 4)
        XCTAssertEqual(clipped?.width, 50)
        XCTAssertEqual(clipped?.height, 16)

        XCTAssertNil(
            MetalPresenter.clipCopyRect(
                sourceX: 64, sourceY: 0, destX: 0, destY: 0,
                width: 8, height: 8,
                sourceWidth: 64, sourceHeight: 32,
                destWidth: 64, destHeight: 32))
    }
}
