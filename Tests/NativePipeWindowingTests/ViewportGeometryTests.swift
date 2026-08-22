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

        let layer = WindowBridge.surfaceLayerGeometry(
            frame: frame, allocationSize: CGSize(width: 1_600, height: 1_200))
        XCTAssertEqual(layer.bounds, CGRect(x: 0, y: 0, width: 800, height: 600))
        XCTAssertEqual(layer.contentsRect, CGRect(x: 0, y: 0, width: 1, height: 1))
        XCTAssertEqual(layer.contentsScale, 2)

        let coordinates = SurfaceCoordinateSpace(
            .init(x: 26, y: 23, width: 800, height: 600))
        // The guest output is already cropped to window_geometry. Its host
        // scene starts at zero, while input still restores the surface origin.
        XCTAssertEqual(coordinates.sceneBounds, CGRect(x: 0, y: 0, width: 800, height: 600))
        XCTAssertEqual(
            coordinates.surfacePoint(fromContent: .zero),
            CGPoint(x: 26, y: 23))
        XCTAssertEqual(
            coordinates.contentPoint(fromSurface: CGPoint(x: 36, y: 30)),
            CGPoint(x: 10, y: 7))
        XCTAssertEqual(
            coordinates.contentPoint(
                fromSurface: coordinates.surfacePoint(
                    fromContent: CGPoint(x: 734.5, y: 418.25))),
            CGPoint(x: 734.5, y: 418.25))

        // While AppKit is ahead of the client during live resize, the whole
        // committed tree is stretched and pointer input uses the exact inverse.
        let resizing = SurfaceCoordinateSpace(
            .init(x: 26, y: 23, width: 800, height: 600),
            contentSize: CGSize(width: 400, height: 300))
        XCTAssertEqual(resizing.sceneScale, CGSize(width: 0.5, height: 0.5))
        XCTAssertEqual(
            resizing.surfacePoint(fromContent: CGPoint(x: 200, y: 150)),
            CGPoint(x: 426, y: 323))
        XCTAssertEqual(
            resizing.contentPoint(fromSurface: CGPoint(x: 426, y: 323)),
            CGPoint(x: 200, y: 150))
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

    func testCapacityIOSurfaceCropUsesAllocationDimensions() {
        let frame = Windowing.Frame(
            resourceID: 9, width: 800, height: 500,
            bytesPerRow: 4_096, format: .bgra8888)

        XCTAssertEqual(
            frame.contentsRect(
                for: CGRect(x: 0, y: 0, width: 800, height: 500),
                allocationSize: CGSize(width: 1_024, height: 640)),
            CGRect(x: 0, y: 0, width: 0.78125, height: 0.78125))
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

    func testMissingOutputIOSurfaceIsDeferredRatherThanReleased() {
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

        XCTAssertFalse(commands.contains {
            if case .framePresented(surface: 8, presentationID: 17) = $0 { return true }
            return false
        })
        bridge.closeAll()
    }

    func testSupersededDeferredFrameCompletesAndReleasesItsRingSlot() {
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        bridge.apply(.surfaceCreated(surface: 8))
        bridge.apply(.toplevelCreated(window: 3, surface: 8))

        for presentationID in [UInt32(17), 18] {
            bridge.apply(.committed(
                surface: 8,
                frame: Windowing.Frame(
                    resourceID: 99, width: 64, height: 48, bytesPerRow: 256,
                    format: .bgra8888, presentationID: presentationID)))
        }

        XCTAssertTrue(commands.contains {
            if case .framePresented(surface: 8, presentationID: 17) = $0 { return true }
            return false
        })
        XCTAssertTrue(commands.contains {
            if case .frameReleased(surface: 8, presentationID: 17) = $0 { return true }
            return false
        })
        XCTAssertFalse(commands.contains {
            if case .frameReleased(surface: 8, presentationID: 18) = $0 { return true }
            return false
        })
        bridge.closeAll()
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

}
