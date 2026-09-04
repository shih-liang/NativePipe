import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class ViewportGeometryTests: XCTestCase {
	func testCommittedLayerStaysAtVisualTopLeftOfFlippedContainer() {
		XCTAssertEqual(
			SurfaceLayerPlacement.topLeftPosition(
				container: CGSize(width: 800, height: 600),
				child: CGSize(width: 640, height: 480)),
			CGPoint(x: 0, y: 120))
		XCTAssertEqual(
			SurfaceLayerPlacement.topLeftPosition(
				container: CGSize(width: 640, height: 480),
				child: CGSize(width: 800, height: 600)),
			CGPoint(x: 0, y: -120))
	}

	func testClientDecoratedToplevelUsesExactCommittedGeometry() throws {
		final class TextureSource: FrameSource {
			let device: MTLDevice

			init(device: MTLDevice) { self.device = device }

			func surface(forResource resourceID: UInt32) -> IOSurfaceRef? { nil }

			func metalTextures(
				for layers: [Windowing.SceneLayer],
				completion: @escaping @MainActor ([FrameTextureResolution]) -> Void
			) {
				completion(layers.map { layer in
					let descriptor = MTLTextureDescriptor.texture2DDescriptor(
						pixelFormat: .bgra8Unorm, width: layer.width,
						height: layer.height, mipmapped: false)
					descriptor.usage = [.shaderRead]
					return FrameTextureResolution(
						status: .ready, texture: device.makeTexture(descriptor: descriptor))
				})
			}
		}

		guard let device = MTLCreateSystemDefaultDevice() else {
			throw XCTSkip("Metal is unavailable")
		}
		let bridge = WindowBridge(frameSource: TextureSource(device: device))
		bridge.apply(.surfaceCreated(surface: 8))
		bridge.apply(.toplevelCreated(window: 3, surface: 8))
		var committed = scene(presentationID: 1)
		committed.width = 1_336
		committed.height = 1_040
		committed.scale = 2
		committed.windowGeometry = .init(x: 14, y: 12, width: 640, height: 491)
		committed.layers[0].width = 1_336
		committed.layers[0].height = 1_040
		committed.layers[0].bytesPerRow = 5_344
		committed.layers[0].destination = .init(
			x: 0, y: 0, width: 1_336, height: 1_040)
		committed.layers[0].sourcePixels = .init(
			x: 0, y: 0, width: 1_336, height: 1_040)
		committed.layers[0].clip = .init(
			x: 0, y: 0, width: 1_336, height: 1_040)

		bridge.apply(.sceneCommitted(scene: committed))

		XCTAssertEqual(
			bridge.window(3)?.window?.contentView?.bounds.size,
			NSSize(width: 640, height: 491))
		let initialTop = try XCTUnwrap(bridge.window(3)?.window).frame.maxY

		var resized = committed
		resized.presentationID = 2
		resized.configureSerial = 7
		resized.width = 1_220
		resized.height = 860
		resized.windowGeometry = .init(x: 14, y: 12, width: 610, height: 430)
		resized.layers[0].resourceID = 2
		resized.layers[0].width = 1_220
		resized.layers[0].height = 860
		resized.layers[0].bytesPerRow = 4_880
		resized.layers[0].destination = .init(
			x: 0, y: 0, width: 1_220, height: 860)
		resized.layers[0].sourcePixels = resized.layers[0].destination
		resized.layers[0].clip = resized.layers[0].destination
		bridge.apply(.sceneCommitted(scene: resized))

		let resizedWindow = try XCTUnwrap(bridge.window(3)?.window)
		XCTAssertEqual(
			resizedWindow.contentView?.bounds.size,
			NSSize(width: 610, height: 430))
		XCTAssertEqual(resizedWindow.frame.maxY, initialTop, accuracy: 0.5)
		bridge.closeAll()
	}

	func testResizeAnchorTracksTheOppositeAppKitEdges() {
		let start = CGRect(x: 100, y: 200, width: 600, height: 500)
		let bottomRight = CGRect(x: 100, y: 140, width: 680, height: 560)
		let anchor = WindowFrameAnchor.inferred(from: start, to: bottomRight)
		XCTAssertEqual(anchor, .topLeft)
		XCTAssertEqual(
			anchor.frame(size: CGSize(width: 650, height: 540), relativeTo: bottomRight),
			CGRect(x: 100, y: 160, width: 650, height: 540))

		let topLeft = CGRect(x: 60, y: 200, width: 640, height: 550)
		let opposite = WindowFrameAnchor.inferred(from: start, to: topLeft)
		XCTAssertEqual(
			opposite.frame(size: CGSize(width: 620, height: 520), relativeTo: topLeft),
			CGRect(x: 80, y: 200, width: 620, height: 520))
	}

	func testResizeEndAlwaysSendsFreshFinalConfigureWithoutResizing() throws {
		let bridge = WindowBridge(frameSource: nil)
		var commands: [Windowing.HostCommand] = []
		bridge.output = { commands.append($0) }
		bridge.apply(.surfaceCreated(surface: 8))
		bridge.apply(.toplevelCreated(window: 3, surface: 8))
		let native = try XCTUnwrap(bridge.window(3))
		native.revealToplevel(width: 640, height: 480)
		let window = try XCTUnwrap(native.window)
		commands.removeAll()

		for _ in 0..<2 {
			native.windowWillStartLiveResize(
				Notification(name: NSWindow.willStartLiveResizeNotification,
				             object: window))
			native.windowDidEndLiveResize(
				Notification(name: NSWindow.didEndLiveResizeNotification,
				             object: window))
		}

		let configures = commands.compactMap { command -> (Windowing.Size, [Windowing.ToplevelState], UInt32)? in
			guard case .configure(_, let size, let states, let serial) = command else {
				return nil
			}
			return (size, states, serial)
		}
		XCTAssertEqual(configures.count, 2)
		XCTAssertEqual(configures[0].0, configures[1].0)
		XCTAssertFalse(configures[0].1.contains(.resizing))
		XCTAssertFalse(configures[1].1.contains(.resizing))
		XCTAssertNotEqual(configures[0].2, configures[1].2)
		bridge.closeAll()
	}

	func testLatestSceneCarriesDamageFromSkippedUnencodedScene() {
		let layer = Windowing.SceneLayer(
			surface: 1, resourceID: 10, width: 800, height: 600,
			bytesPerRow: 3_200, format: .bgra8888,
			destination: .init(x: 0, y: 0, width: 800, height: 600),
			sourcePixels: .init(x: 0, y: 0, width: 800, height: 600),
			clip: .init(x: 0, y: 0, width: 800, height: 600),
			alpha: 1, opaque: true, transform: .normal)
		let older = Windowing.SceneSnapshot(
			surface: 1, presentationID: 1, width: 800, height: 600, scale: 1,
			windowGeometry: .init(x: 0, y: 0, width: 800, height: 600),
			layers: [layer], damage: [.init(x: 10, y: 20, width: 30, height: 40)])
		var newerLayer = layer
		newerLayer.resourceID = 11
		let newer = Windowing.SceneSnapshot(
			surface: 1, presentationID: 2, width: 800, height: 600, scale: 1,
			windowGeometry: older.windowGeometry, layers: [newerLayer],
			damage: [.init(x: 100, y: 90, width: 20, height: 10)])

		let merged = newer.includingUnrenderedDamage(from: older)
		XCTAssertEqual(merged.presentationID, 2)
		XCTAssertEqual(merged.layers[0].resourceID, 11)
		XCTAssertEqual(
			merged.damage, [.init(x: 10, y: 20, width: 110, height: 80)])
	}

    func testSkippedSceneWithDifferentGeometryForcesFullRedraw() {
        let old = scene(presentationID: 1)
        var new = scene(presentationID: 2)
        new.windowGeometry.width -= 1
        new.damage = []

        XCTAssertEqual(
            new.includingUnrenderedDamage(from: old).damage,
            [.init(x: 0, y: 0, width: new.width, height: new.height)])
    }

	func testDrawableAgeAccumulatesDamageForThreeDrawableRotation() {
		var tracker = DrawableAgeTracker(capacity: 3)
		var frame = scene(presentationID: 1)
		frame.damage = [.init(x: 1, y: 1, width: 2, height: 2)]
		let first = tracker.plan(
			drawableID: 10, scene: frame, drawableWidth: 64, drawableHeight: 48)
		XCTAssertTrue(first.redrawAll)
		tracker.commit(first)

		frame.presentationID = 2
		frame.damage = [.init(x: 10, y: 10, width: 2, height: 2)]
		let second = tracker.plan(
			drawableID: 11, scene: frame, drawableWidth: 64, drawableHeight: 48)
		tracker.commit(second)

		frame.presentationID = 3
		frame.damage = [.init(x: 20, y: 5, width: 3, height: 3)]
		let third = tracker.plan(
			drawableID: 12, scene: frame, drawableWidth: 64, drawableHeight: 48)
		tracker.commit(third)

		frame.presentationID = 4
		frame.damage = [.init(x: 4, y: 30, width: 2, height: 2)]
		let reused = tracker.plan(
			drawableID: 10, scene: frame, drawableWidth: 64, drawableHeight: 48)
		XCTAssertFalse(reused.redrawAll)
		XCTAssertEqual(
			reused.damage, [.init(x: 4, y: 5, width: 19, height: 27)])
	}

	func testDrawableAgeFallsBackToFullAfterHistoryExpires() {
		var tracker = DrawableAgeTracker(capacity: 2)
		var frame = scene(presentationID: 1)
		for id: UInt64 in 1...3 {
			frame.presentationID = UInt32(id)
			frame.damage = [.init(x: Int(id), y: 0, width: 1, height: 1)]
			let plan = tracker.plan(
				drawableID: id, scene: frame, drawableWidth: 64, drawableHeight: 48)
			tracker.commit(plan)
		}
		let expired = tracker.plan(
			drawableID: 1, scene: frame, drawableWidth: 64, drawableHeight: 48)
		XCTAssertTrue(expired.redrawAll)
		XCTAssertEqual(expired.damage, [.init(x: 0, y: 0, width: 64, height: 48)])
	}

	func testDrawableAgeResizeForcesFullRedraw() {
		var tracker = DrawableAgeTracker(capacity: 3)
		let frame = scene(presentationID: 1)
		let first = tracker.plan(
			drawableID: 7, scene: frame, drawableWidth: 64, drawableHeight: 48)
		tracker.commit(first)

		let resized = tracker.plan(
			drawableID: 7, scene: frame, drawableWidth: 80, drawableHeight: 48)
		XCTAssertTrue(resized.redrawAll)
		XCTAssertEqual(resized.damage, [.init(x: 0, y: 0, width: 80, height: 48)])
	}

    func testPopupConstraintsPreferFlipThenSlideAndResize() {
        let bounds = CGRect(x: 0, y: 0, width: 800, height: 600)
        let flipped = Windowing.PopupPlacement(
            window: 2, parent: 1,
            x: 740, y: 20, flippedX: 500, flippedY: 20,
            width: 240, height: 180, adjustment: 4,
            token: 1, reactive: false)
        XCTAssertEqual(
            WindowBridge.constrainPopup(flipped, to: bounds),
            CGRect(x: 500, y: 20, width: 240, height: 180))

        var slide = flipped
        slide.adjustment = 1
        XCTAssertEqual(
            WindowBridge.constrainPopup(slide, to: bounds),
            CGRect(x: 560, y: 20, width: 240, height: 180))

        var resize = flipped
        resize.x = -20
        resize.width = 900
        resize.adjustment = 16
        XCTAssertEqual(
            WindowBridge.constrainPopup(resize, to: bounds),
            CGRect(x: 0, y: 20, width: 800, height: 180))
    }

    func testPopupTracksCommittedWindowGeometryAfterFirstScene() throws {
        final class TextureSource: FrameSource {
            let device: MTLDevice

            init(device: MTLDevice) { self.device = device }

            func surface(forResource resourceID: UInt32) -> IOSurfaceRef? { nil }

            func metalTextures(
                for layers: [Windowing.SceneLayer],
                completion: @escaping @MainActor ([FrameTextureResolution]) -> Void
            ) {
                completion(layers.map { layer in
                    let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                        pixelFormat: .bgra8Unorm, width: layer.width,
                        height: layer.height, mipmapped: false)
                    descriptor.usage = [.shaderRead]
                    guard let texture = device.makeTexture(descriptor: descriptor) else {
                        return FrameTextureResolution(status: .unavailable)
                    }
                    return FrameTextureResolution(status: .ready, texture: texture)
                })
            }
        }

        guard let device = MTLCreateSystemDefaultDevice() else {
            throw XCTSkip("Metal is unavailable")
        }
        let bridge = WindowBridge(frameSource: TextureSource(device: device))
        bridge.apply(.surfaceCreated(surface: 1))
        bridge.apply(.toplevelCreated(window: 8, surface: 1))
        bridge.apply(.surfaceCreated(surface: 12))
        bridge.apply(.popupCreated(
            window: 14, surface: 12, parent: 8,
            x: 373, y: 182, width: 152, height: 218))

        func popupScene(
            presentationID: UInt32, width: Int, height: Int
        ) -> Windowing.SceneSnapshot {
            Windowing.SceneSnapshot(
                surface: 12, presentationID: presentationID,
                width: width * 2, height: height * 2, scale: 2,
                windowGeometry: .init(x: 0, y: 0, width: width, height: height),
                layers: [.init(
                    surface: 12, resourceID: presentationID,
                    width: width * 2, height: height * 2,
                    bytesPerRow: width * 8, format: .bgra8888,
                    destination: .init(
                        x: 0, y: 0, width: Double(width * 2),
                        height: Double(height * 2)),
                    sourcePixels: .init(
                        x: 0, y: 0, width: Double(width * 2),
                        height: Double(height * 2)),
                    clip: .init(
                        x: 0, y: 0, width: Double(width * 2),
                        height: Double(height * 2)))],
                damage: [.init(x: 0, y: 0, width: width * 2, height: height * 2)])
        }

        bridge.apply(.sceneCommitted(
            scene: popupScene(presentationID: 1, width: 152, height: 218)))
        XCTAssertEqual(
            bridge.window(14)?.window?.contentView?.bounds.size,
            NSSize(width: 152, height: 218))

        bridge.apply(.sceneCommitted(
            scene: popupScene(presentationID: 2, width: 197, height: 220)))
        XCTAssertEqual(
            bridge.window(14)?.window?.contentView?.bounds.size,
            NSSize(width: 197, height: 220))
        bridge.closeAll()
    }

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
        // The guest output and host input protocol are both zero-origin within
        // window_geometry. Only the guest maps that point back to wl_surface.
        XCTAssertEqual(coordinates.sceneBounds, CGRect(x: 0, y: 0, width: 800, height: 600))
        XCTAssertEqual(
            coordinates.windowPoint(fromContent: .zero),
            .zero)
        XCTAssertEqual(
            coordinates.contentPoint(fromWindow: CGPoint(x: 10, y: 7)),
            CGPoint(x: 10, y: 7))
        XCTAssertEqual(
            coordinates.contentPoint(
                fromWindow: coordinates.windowPoint(
                    fromContent: CGPoint(x: 734.5, y: 418.25))),
            CGPoint(x: 734.5, y: 418.25))

        // A live resize never rescales the committed scene or pointer space.
        // The client receives the newest configure and paints that exact size.
        let resizing = SurfaceCoordinateSpace(
            .init(x: 26, y: 23, width: 800, height: 600))
        XCTAssertEqual(
            resizing.windowPoint(fromContent: CGPoint(x: 200, y: 150)),
            CGPoint(x: 200, y: 150))
        XCTAssertEqual(
            resizing.contentPoint(fromWindow: CGPoint(x: 200, y: 150)),
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

    func testCustomCursorImageIsFlippedWithinItsSourceRect() {
        let source = CGRect(x: 0, y: 0, width: 1, height: 2)
        let lower = CIImage(color: .red).cropped(
            to: CGRect(x: 0, y: 0, width: 1, height: 1))
        let upper = CIImage(color: .blue).cropped(
            to: CGRect(x: 0, y: 1, width: 1, height: 1))
        let image = upper.composited(over: lower)

        let flipped = WindowBridge.topDownCursorImage(image, source: source)

        XCTAssertEqual(flipped.extent, source)
        let context = CIContext(options: [.useSoftwareRenderer: true])
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        func pixels(_ input: CIImage) -> [UInt8] {
            var result = [UInt8](repeating: 0, count: 8)
            context.render(
                input, toBitmap: &result, rowBytes: 4, bounds: source,
                format: .RGBA8, colorSpace: colorSpace)
            return result
        }
        let before = pixels(image)
        let after = pixels(flipped)
        XCTAssertEqual(Array(after[0..<4]), Array(before[4..<8]))
        XCTAssertEqual(Array(after[4..<8]), Array(before[0..<4]))
    }

    func testMissingSceneTextureIsDeferredRatherThanReleased() {
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        bridge.apply(.surfaceCreated(surface: 8))
        bridge.apply(.toplevelCreated(window: 3, surface: 8))
        bridge.apply(.sceneCommitted(scene: scene(presentationID: 17)))

        XCTAssertFalse(commands.contains {
            if case .framePresented(surface: 8, presentationID: 17) = $0 { return true }
            return false
        })
        bridge.closeAll()
    }

    func testUnmapClosesOnlyThePhysicalWindowAndRetainsRole() {
        let bridge = WindowBridge(frameSource: nil)
        bridge.apply(.surfaceCreated(surface: 8))
        bridge.apply(.toplevelCreated(window: 3, surface: 8))
        let role = bridge.window(3)

        bridge.apply(.surfaceUnmapped(surface: 8))

        XCTAssertNotNil(role)
        XCTAssertTrue(bridge.window(3) === role)
        bridge.apply(.toplevelDestroyed(window: 3))
        XCTAssertNil(bridge.window(3))
        bridge.closeAll()
    }

    func testSupersededDeferredSceneReleasesSourceButDefersItsLatch() {
        let bridge = WindowBridge(frameSource: nil)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        bridge.apply(.surfaceCreated(surface: 8))
        bridge.apply(.toplevelCreated(window: 3, surface: 8))

        for presentationID in [UInt32(17), 18] {
            bridge.apply(.sceneCommitted(scene: scene(presentationID: presentationID)))
        }

        XCTAssertFalse(commands.contains {
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
        XCTAssertTrue(commands.contains {
            if case .framePresented(surface: 8, presentationID: 17) = $0 { return true }
            return false
        })
        XCTAssertTrue(commands.contains {
            if case .framePresented(surface: 8, presentationID: 18) = $0 { return true }
            return false
        })
    }

    func testPublishedButUnpresentableSceneIsRetiredWithoutDeadlockingFIFO() {
        final class PublishedFrameSource: FrameSource {
            func surface(forResource resourceID: UInt32) -> IOSurfaceRef? { nil }
            func isResourcePublished(_ resourceID: UInt32) -> Bool { resourceID == 99 }
        }

        let bridge = WindowBridge(frameSource: PublishedFrameSource())
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }
        bridge.apply(.surfaceCreated(surface: 8))
        bridge.apply(.toplevelCreated(window: 3, surface: 8))
        bridge.apply(.sceneCommitted(scene: scene(presentationID: 17)))

        XCTAssertTrue(commands.contains {
            if case .framePresented(surface: 8, presentationID: 17) = $0 { return true }
            return false
        })
        XCTAssertTrue(commands.contains {
            if case .frameReleased(surface: 8, presentationID: 17) = $0 { return true }
            return false
        })
        bridge.closeAll()
    }

    func testOldTextureLookupCannotConsumeNewSurfaceGeneration() {
        final class DeferredFrameSource: FrameSource {
            typealias Completion = @MainActor ([FrameTextureResolution]) -> Void
            var completions: [Completion] = []

            func surface(forResource resourceID: UInt32) -> IOSurfaceRef? { nil }
            func metalTextures(
                for layers: [Windowing.SceneLayer],
                completion: @escaping Completion
            ) {
                completions.append(completion)
            }
        }

        let source = DeferredFrameSource()
        let bridge = WindowBridge(frameSource: source)
        var commands: [Windowing.HostCommand] = []
        bridge.output = { commands.append($0) }

        bridge.apply(.surfaceCreated(surface: 8))
        bridge.apply(.toplevelCreated(window: 3, surface: 8))
        bridge.apply(.sceneCommitted(scene: scene(presentationID: 17)))
        XCTAssertEqual(source.completions.count, 1)

        bridge.apply(.surfaceDestroyed(surface: 8))
        bridge.apply(.surfaceCreated(surface: 8))
        bridge.apply(.toplevelCreated(window: 4, surface: 8))
        bridge.apply(.sceneCommitted(scene: scene(presentationID: 18)))
        XCTAssertEqual(source.completions.count, 2)

        source.completions[0]([
            FrameTextureResolution(status: .unavailable)
        ])
        XCTAssertFalse(commands.contains {
            if case .frameReleased(surface: 8, presentationID: 18) = $0 { return true }
            return false
        })

        source.completions[1]([
            FrameTextureResolution(status: .unavailable)
        ])
        XCTAssertTrue(commands.contains {
            if case .frameReleased(surface: 8, presentationID: 18) = $0 { return true }
            return false
        })
        XCTAssertTrue(commands.contains {
            if case .framePresented(surface: 8, presentationID: 18) = $0 { return true }
            return false
        })
        bridge.closeAll()
    }

    private func scene(presentationID: UInt32) -> Windowing.SceneSnapshot {
        Windowing.SceneSnapshot(
            surface: 8, presentationID: presentationID,
            width: 64, height: 48, scale: 1,
            windowGeometry: .init(x: 0, y: 0, width: 64, height: 48),
            layers: [.init(
                surface: 8, resourceID: 99,
                width: 64, height: 48, bytesPerRow: 256,
                format: .bgra8888,
                destination: .init(x: 0, y: 0, width: 64, height: 48),
                sourcePixels: .init(x: 0, y: 0, width: 64, height: 48),
                clip: .init(x: 0, y: 0, width: 64, height: 48),
                opaque: true)])
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
