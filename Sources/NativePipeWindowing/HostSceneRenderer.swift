@preconcurrency import Metal
import NativePipeProtocol
import QuartzCore

/// A resolved layer keeps the exact Metal object alive until the command buffer
/// finishes reading it. No texture or backing storage is created here.
struct ResolvedSceneLayer {
    let state: Windowing.SceneLayer
    let texture: MTLTexture
}

extension Windowing.SceneSnapshot {
    /// A latest-value queue may discard an unencoded scene, but its damage is
    /// still part of the transition recorded for the drawable pool. The newest
    /// layer list is authoritative; only the damaged output area is carried
    /// forward.
    func includingUnrenderedDamage(from older: Self) -> Self {
		guard width == older.width, height == older.height,
              scale == older.scale, windowGeometry == older.windowGeometry else {
            var result = self
            result.damage = [Windowing.Rect(x: 0, y: 0, width: width, height: height)]
            return result
        }
        let rectangles = older.damage + damage
        guard let first = rectangles.first else { return self }
        var left = first.x
        var top = first.y
        var right = first.x + first.width
        var bottom = first.y + first.height
        for rect in rectangles.dropFirst() {
            left = min(left, rect.x)
            top = min(top, rect.y)
            right = max(right, rect.x + rect.width)
            bottom = max(bottom, rect.y + rect.height)
        }
        var result = self
        result.damage = [Windowing.Rect(
            x: left, y: top, width: right - left, height: bottom - top)]
        return result
    }
}

/// Tracks only the recent damage needed to restore each CAMetalLayer drawable.
/// No extra texture is retained: the layer's own drawable pool is the backing
/// store, identified by Metal's stable GPU resource id.
struct DrawableAgeTracker {
    struct Geometry: Equatable {
        let drawableWidth: Int
        let drawableHeight: Int
        let sceneWidth: Int
        let sceneHeight: Int
        let scale: Int
        let windowGeometry: Windowing.Rect
    }

    struct Plan {
        let serial: UInt64
        let drawableID: UInt64
        let geometry: Geometry
        let redrawAll: Bool
        let damage: [Windowing.Rect]
        fileprivate let transitionDamage: [Windowing.Rect]
    }

    private struct Slot {
        let serial: UInt64
        let geometry: Geometry
    }

    private struct Record {
        let serial: UInt64
        let damage: [Windowing.Rect]
    }

    private let capacity: Int
    private var serial: UInt64 = 0
    private var lastGeometry: Geometry?
    private var slots: [UInt64: Slot] = [:]
    private var history: [Record] = []

    init(capacity: Int) {
        self.capacity = max(1, capacity)
    }

    mutating func plan(
        drawableID: UInt64, scene: Windowing.SceneSnapshot,
        drawableWidth: Int, drawableHeight: Int
    ) -> Plan {
        let geometry = Geometry(
            drawableWidth: drawableWidth, drawableHeight: drawableHeight,
            sceneWidth: scene.width, sceneHeight: scene.height,
            scale: scene.scale, windowGeometry: scene.windowGeometry)
        let full = [Windowing.Rect(
            x: 0, y: 0, width: drawableWidth, height: drawableHeight)]
        let transition = lastGeometry == geometry
            ? Self.coalesced(scene.damage, width: drawableWidth, height: drawableHeight)
            : full
        let nextSerial = serial &+ 1

        var redrawAll = true
        var damage = full
        if let slot = slots[drawableID], slot.geometry == geometry {
            let firstRequired = slot.serial &+ 1
            let historyStartsInTime = slot.serial == serial ||
                (history.first.map { $0.serial <= firstRequired } ?? false)
            if historyStartsInTime {
                redrawAll = false
                let accumulated = history.filter { $0.serial > slot.serial }
                    .flatMap(\.damage) + transition
                damage = Self.coalesced(
                    accumulated,
                    width: drawableWidth, height: drawableHeight)
            }
        }

        return Plan(
            serial: nextSerial, drawableID: drawableID, geometry: geometry,
            redrawAll: redrawAll, damage: damage,
            transitionDamage: transition)
    }

    mutating func commit(_ plan: Plan) {
        precondition(plan.serial == serial &+ 1)
        serial = plan.serial
        lastGeometry = plan.geometry
        slots[plan.drawableID] = Slot(serial: serial, geometry: plan.geometry)
        history.append(Record(serial: serial, damage: plan.transitionDamage))
        if history.count > capacity {
            history.removeFirst(history.count - capacity)
        }
        if slots.count > capacity {
            let keep = slots.sorted { $0.value.serial > $1.value.serial }.prefix(capacity)
            slots = Dictionary(uniqueKeysWithValues: keep.map { ($0.key, $0.value) })
        }
    }

    mutating func invalidate() {
        serial = 0
        lastGeometry = nil
        slots.removeAll(keepingCapacity: true)
        history.removeAll(keepingCapacity: true)
    }

    private static func coalesced<S: Sequence>(
        _ rectangles: S, width: Int, height: Int
    ) -> [Windowing.Rect] where S.Element == Windowing.Rect {
        var bounds: Windowing.Rect?
        for rect in rectangles {
            let x0 = max(0, rect.x)
            let y0 = max(0, rect.y)
            let x1 = min(width, rect.x + rect.width)
            let y1 = min(height, rect.y + rect.height)
            guard x1 > x0, y1 > y0 else { continue }
            if let old = bounds {
                let right = max(old.x + old.width, x1)
                let bottom = max(old.y + old.height, y1)
                let left = min(old.x, x0)
                let top = min(old.y, y0)
                bounds = Windowing.Rect(
                    x: left, y: top, width: right - left, height: bottom - top)
            } else {
                bounds = Windowing.Rect(x: x0, y: y0, width: x1 - x0, height: y1 - y0)
            }
        }
        return bounds.map { [$0] } ?? []
    }
}

/// Composites a guest-resolved Wayland scene straight into one drawable.
/// Textures are bound conventionally, one draw call per layer; this deliberately
/// avoids argument-buffer descriptors on the NativePipe boundary.
final class HostSceneRenderer: @unchecked Sendable {
    enum RendererError: Error {
        case shaderLibrary
        case pipeline
        case sampler
        case commandBuffer
        case encoder
        case incompatibleTexture
    }

    private struct VertexUniforms {
        var destination: SIMD4<Float>
        var outputSize: SIMD2<Float>
        var padding = SIMD2<Float>(repeating: 0)
    }

    private struct FragmentUniforms {
        var sourcePixels: SIMD4<Float>
        var textureSize: SIMD2<Float>
        var alpha: Float
        var opaque: UInt32
        var transform: UInt32
        var padding: UInt32 = 0
    }

    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let clearPipeline: MTLRenderPipelineState
    private let sampler: MTLSamplerState

    init(device: MTLDevice) throws {
        guard let queue = device.makeCommandQueue() else { throw RendererError.commandBuffer }
        self.queue = queue

        let source = """
        #include <metal_stdlib>
        using namespace metal;

        struct VertexUniforms {
            float4 destination;
            float2 outputSize;
            float2 padding;
        };
        struct FragmentUniforms {
            float4 sourcePixels;
            float2 textureSize;
            float alpha;
            uint opaque;
            uint transform;
            uint padding;
        };
        struct RasterData {
            float4 position [[position]];
            float2 unitUV;
        };

        vertex RasterData np_scene_vertex(
            uint vertexID [[vertex_id]], constant VertexUniforms &u [[buffer(0)]]) {
            constexpr float2 corners[4] = {
                float2(0.0, 0.0), float2(1.0, 0.0),
                float2(0.0, 1.0), float2(1.0, 1.0)
            };
            float2 p = corners[vertexID];
            float2 pixel = u.destination.xy + p * u.destination.zw;
            RasterData out;
            out.position = float4(
                pixel.x / u.outputSize.x * 2.0 - 1.0,
                1.0 - pixel.y / u.outputSize.y * 2.0,
                0.0, 1.0);
            out.unitUV = p;
            return out;
        }

        static float2 transformed_uv(float2 uv, uint transform) {
            switch (transform) {
                case 1: return float2(uv.y, 1.0 - uv.x);
                case 2: return float2(1.0 - uv.x, 1.0 - uv.y);
                case 3: return float2(1.0 - uv.y, uv.x);
                case 4: return float2(1.0 - uv.x, uv.y);
                case 5: return float2(uv.y, uv.x);
                case 6: return float2(uv.x, 1.0 - uv.y);
                case 7: return float2(1.0 - uv.y, 1.0 - uv.x);
                default: return uv;
            }
        }

        fragment float4 np_scene_fragment(
            RasterData in [[stage_in]], texture2d<float> source [[texture(0)]],
            sampler sampleState [[sampler(0)]],
            constant FragmentUniforms &u [[buffer(0)]]) {
            float2 unit = transformed_uv(in.unitUV, u.transform);
            float2 pixel = u.sourcePixels.xy + unit * u.sourcePixels.zw;
            float4 color = source.sample(sampleState, pixel / u.textureSize);
            if (u.opaque != 0) color.a = 1.0;
            return color * u.alpha;
        }

        fragment float4 np_scene_clear() {
            return float4(0.0);
        }
        """
        guard let library = try? device.makeLibrary(source: source, options: nil),
              let vertex = library.makeFunction(name: "np_scene_vertex"),
              let fragment = library.makeFunction(name: "np_scene_fragment")
        else { throw RendererError.shaderLibrary }

        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex
        descriptor.fragmentFunction = fragment
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        descriptor.colorAttachments[0].isBlendingEnabled = true
        descriptor.colorAttachments[0].rgbBlendOperation = .add
        descriptor.colorAttachments[0].alphaBlendOperation = .add
        descriptor.colorAttachments[0].sourceRGBBlendFactor = .one
        descriptor.colorAttachments[0].sourceAlphaBlendFactor = .one
        descriptor.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
        descriptor.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
        guard let pipeline = try? device.makeRenderPipelineState(descriptor: descriptor) else {
            throw RendererError.pipeline
        }
        self.pipeline = pipeline

		let clearDescriptor = MTLRenderPipelineDescriptor()
		clearDescriptor.vertexFunction = vertex
		clearDescriptor.fragmentFunction = library.makeFunction(name: "np_scene_clear")
		clearDescriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
		guard let clearPipeline = try? device.makeRenderPipelineState(
			descriptor: clearDescriptor) else {
			throw RendererError.pipeline
		}
		self.clearPipeline = clearPipeline

        let samplerDescriptor = MTLSamplerDescriptor()
        samplerDescriptor.minFilter = .linear
        samplerDescriptor.magFilter = .linear
        samplerDescriptor.sAddressMode = .clampToEdge
        samplerDescriptor.tAddressMode = .clampToEdge
        guard let sampler = device.makeSamplerState(descriptor: samplerDescriptor) else {
            throw RendererError.sampler
        }
        self.sampler = sampler
    }

    func encode(
        scene: Windowing.SceneSnapshot, layers: [ResolvedSceneLayer],
        damage: [Windowing.Rect], redrawAll: Bool,
        drawable: CAMetalDrawable,
        completion: @escaping (MTLCommandBuffer) -> Void
    ) throws {
        guard layers.count == scene.layers.count,
              let command = queue.makeCommandBuffer() else {
            throw RendererError.commandBuffer
        }
        let target = drawable.texture
        guard target.pixelFormat == .bgra8Unorm,
              target.width > 0, target.height > 0 else {
            throw RendererError.incompatibleTexture
        }

		let limitWidth = min(scene.width, target.width)
		let limitHeight = min(scene.height, target.height)
		let regions = redrawAll
			? [MTLScissorRect(x: 0, y: 0, width: limitWidth, height: limitHeight)]
			: damage.compactMap { scissor($0, width: limitWidth, height: limitHeight) }

		if canBlit(scene: scene, layer: layers.first), let layer = layers.first {
			try encodeBlit(
				layer.texture, regions: regions, redrawAll: redrawAll,
				target: target, command: command)
        } else if redrawAll || !regions.isEmpty {
            let pass = MTLRenderPassDescriptor()
			pass.colorAttachments[0].texture = target
			pass.colorAttachments[0].loadAction = redrawAll ? .clear : .load
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0)
            guard let encoder = command.makeRenderCommandEncoder(descriptor: pass) else {
                throw RendererError.encoder
            }
			for region in regions {
				if !redrawAll {
					encodeClear(region: region, outputWidth: target.width,
						outputHeight: target.height, encoder: encoder)
				}
				encoder.setRenderPipelineState(pipeline)
				encoder.setFragmentSamplerState(sampler, index: 0)
				for layer in layers {
					try encode(
						layer: layer, scene: scene,
						outputWidth: target.width, outputHeight: target.height,
						damage: region, encoder: encoder)
				}
            }
            encoder.endEncoding()
        }

        command.addCompletedHandler { command in
            // Retain every source wrapper until Metal has completed its reads.
            withExtendedLifetime(layers) {}
            completion(command)
        }
        command.present(drawable)
        command.commit()
    }

	private func encodeBlit(
		_ source: MTLTexture, regions: [MTLScissorRect], redrawAll: Bool,
		target: MTLTexture, command: MTLCommandBuffer
	) throws {
		if redrawAll && (regions.first?.width != target.width ||
			regions.first?.height != target.height) {
			let pass = MTLRenderPassDescriptor()
			pass.colorAttachments[0].texture = target
			pass.colorAttachments[0].loadAction = .clear
			pass.colorAttachments[0].storeAction = .store
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0)
			guard let clear = command.makeRenderCommandEncoder(descriptor: pass) else {
				throw RendererError.encoder
			}
			clear.endEncoding()
		}

		guard !regions.isEmpty else { return }
		guard let blit = command.makeBlitCommandEncoder() else {
			throw RendererError.encoder
		}
		for region in regions {
			blit.copy(
				from: source, sourceSlice: 0, sourceLevel: 0,
				sourceOrigin: MTLOrigin(x: region.x, y: region.y, z: 0),
				sourceSize: MTLSize(
					width: region.width, height: region.height, depth: 1),
				to: target, destinationSlice: 0, destinationLevel: 0,
				destinationOrigin: MTLOrigin(x: region.x, y: region.y, z: 0))
		}
		blit.endEncoding()
	}

	private func scissor(
		_ rect: Windowing.Rect, width: Int, height: Int
	) -> MTLScissorRect? {
		let x0 = max(0, rect.x)
		let y0 = max(0, rect.y)
		let x1 = min(width, rect.x + rect.width)
		let y1 = min(height, rect.y + rect.height)
		guard x1 > x0, y1 > y0 else { return nil }
		return MTLScissorRect(x: x0, y: y0, width: x1 - x0, height: y1 - y0)
	}

	private func encodeClear(
		region: MTLScissorRect, outputWidth: Int, outputHeight: Int,
		encoder: MTLRenderCommandEncoder
	) {
		var vertex = VertexUniforms(
			destination: SIMD4(0, 0, Float(outputWidth), Float(outputHeight)),
			outputSize: SIMD2(Float(outputWidth), Float(outputHeight)))
		encoder.setRenderPipelineState(clearPipeline)
		encoder.setScissorRect(region)
		encoder.setVertexBytes(
			&vertex, length: MemoryLayout<VertexUniforms>.stride, index: 0)
		encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
	}

    private func encode(
        layer: ResolvedSceneLayer, scene: Windowing.SceneSnapshot,
        outputWidth: Int, outputHeight: Int,
		damage: MTLScissorRect,
        encoder: MTLRenderCommandEncoder
    ) throws {
        let texture = layer.texture
        guard texture.textureType == .type2D, texture.sampleCount == 1,
              texture.width >= layer.state.width,
              texture.height >= layer.state.height else {
            throw RendererError.incompatibleTexture
        }
        let destination = layer.state.destination
        let clip = layer.state.clip
		let x0 = max(damage.x, max(0, Int(clip.x.rounded(.down))))
		let y0 = max(damage.y, max(0, Int(clip.y.rounded(.down))))
		let x1 = min(damage.x + damage.width, min(outputWidth,
			min(scene.width, Int((clip.x + clip.width).rounded(.up)))))
		let y1 = min(damage.y + damage.height, min(outputHeight,
			min(scene.height, Int((clip.y + clip.height).rounded(.up)))))
        guard x1 > x0, y1 > y0 else { return }

        var vertex = VertexUniforms(
            destination: SIMD4(
                Float(destination.x), Float(destination.y),
                Float(destination.width), Float(destination.height)),
            outputSize: SIMD2(Float(outputWidth), Float(outputHeight)))
        let source = layer.state.sourcePixels
        var fragment = FragmentUniforms(
            sourcePixels: SIMD4(
                Float(source.x), Float(source.y),
                Float(source.width), Float(source.height)),
            textureSize: SIMD2(Float(texture.width), Float(texture.height)),
            alpha: layer.state.alpha,
            opaque: layer.state.opaque ? 1 : 0,
            transform: layer.state.transform.rawValue)
        encoder.setScissorRect(MTLScissorRect(
            x: x0, y: y0, width: x1 - x0, height: y1 - y0))
        encoder.setVertexBytes(&vertex, length: MemoryLayout<VertexUniforms>.stride, index: 0)
        encoder.setFragmentBytes(&fragment, length: MemoryLayout<FragmentUniforms>.stride, index: 0)
        encoder.setFragmentTexture(texture, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }

    private func canBlit(
        scene: Windowing.SceneSnapshot, layer: ResolvedSceneLayer?
    ) -> Bool {
        guard let layer else { return false }
        let state = layer.state
        return state.opaque && state.alpha == 1 && state.transform == .normal &&
            state.format == .bgra8888 && layer.texture.pixelFormat == .bgra8Unorm &&
            layer.texture.width == scene.width && layer.texture.height == scene.height &&
            state.destination == .init(
                x: 0, y: 0, width: Double(scene.width), height: Double(scene.height)) &&
            state.sourcePixels == .init(
                x: 0, y: 0, width: Double(scene.width), height: Double(scene.height)) &&
            state.clip == .init(
                x: 0, y: 0, width: Double(scene.width), height: Double(scene.height))
    }
}
