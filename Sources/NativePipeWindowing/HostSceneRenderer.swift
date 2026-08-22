@preconcurrency import Metal
import NativePipeProtocol
import QuartzCore

/// A resolved layer keeps the exact Metal object alive until the command buffer
/// finishes reading it. No texture or backing storage is created here.
struct ResolvedSceneLayer {
    let state: Windowing.SceneLayer
    let texture: MTLTexture
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
        drawable: CAMetalDrawable,
        completion: @escaping (MTLCommandBuffer) -> Void
    ) throws {
        guard layers.count == scene.layers.count,
              let command = queue.makeCommandBuffer() else {
            throw RendererError.commandBuffer
        }
        let target = drawable.texture
        guard target.pixelFormat == .bgra8Unorm,
              target.width == scene.width, target.height == scene.height else {
            throw RendererError.incompatibleTexture
        }

        if canBlit(scene: scene, layer: layers.first) {
            guard let layer = layers.first,
                  let encoder = command.makeBlitCommandEncoder() else {
                throw RendererError.encoder
            }
            encoder.copy(
                from: layer.texture, sourceSlice: 0, sourceLevel: 0,
                sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                sourceSize: MTLSize(width: scene.width, height: scene.height, depth: 1),
                to: target, destinationSlice: 0, destinationLevel: 0,
                destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
            encoder.endEncoding()
        } else {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0)
            guard let encoder = command.makeRenderCommandEncoder(descriptor: pass) else {
                throw RendererError.encoder
            }
            encoder.setRenderPipelineState(pipeline)
            encoder.setFragmentSamplerState(sampler, index: 0)
            for layer in layers {
                try encode(layer: layer, scene: scene, encoder: encoder)
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

    private func encode(
        layer: ResolvedSceneLayer, scene: Windowing.SceneSnapshot,
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
        let x0 = max(0, Int(clip.x.rounded(.down)))
        let y0 = max(0, Int(clip.y.rounded(.down)))
        let x1 = min(scene.width, Int((clip.x + clip.width).rounded(.up)))
        let y1 = min(scene.height, Int((clip.y + clip.height).rounded(.up)))
        guard x1 > x0, y1 > y0 else { return }

        var vertex = VertexUniforms(
            destination: SIMD4(
                Float(destination.x), Float(destination.y),
                Float(destination.width), Float(destination.height)),
            outputSize: SIMD2(Float(scene.width), Float(scene.height)))
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
