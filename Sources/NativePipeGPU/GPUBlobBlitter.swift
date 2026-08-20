import IOSurface
import Metal
import NativePipeProtocol

/// Copies damaged regions from a transient Venus image into a compositor
/// IOSurface blob. The blob is then displayed through `presentCPU`.
enum GPUBlobBlitter {
    static func blit(
        source: MTLTexture,
        lifetime: AnyObject?,
        into resource: GPUResource,
        frame: Windowing.Frame
    ) -> Bool {
        guard let device = MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue(),
              let dest = resource.makeMetalTexture(using: device),
              let command = queue.makeCommandBuffer()
        else { return false }

        if let lifetime { withExtendedLifetime(lifetime) {} }

        let rects = clippedDamageRects(
            frame.damage, width: frame.width, height: frame.height)
        guard !rects.isEmpty else { return false }

        if damageCoversFullFrame(rects, width: frame.width, height: frame.height) {
            clear(dest, on: command)
        }

        encodeCopies(from: source, to: dest, rects: rects, on: command)
        command.commit()
        command.waitUntilCompleted()
        return command.status == .completed
    }

    private static func damageCoversFullFrame(
        _ rects: [(Int, Int, Int, Int)], width: Int, height: Int
    ) -> Bool {
        var covered = 0
        for rect in rects {
            covered += rect.2 * rect.3
        }
        return covered >= width * height
    }

    private static func clippedDamageRects(
        _ damage: [Windowing.Rect], width: Int, height: Int
    ) -> [(Int, Int, Int, Int)] {
        damage.compactMap { rect in
            let x = max(rect.x, 0)
            let y = max(rect.y, 0)
            let w = min(rect.width, width - x)
            let h = min(rect.height, height - y)
            guard w > 0, h > 0 else { return nil }
            return (x, y, w, h)
        }
    }

    private static func clear(_ texture: MTLTexture, on command: MTLCommandBuffer) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0)
        command.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
    }

    private static func encodeCopies(
        from source: MTLTexture, to dest: MTLTexture,
        rects: [(Int, Int, Int, Int)],
        on command: MTLCommandBuffer
    ) {
        guard source.pixelFormat == dest.pixelFormat,
              source.pixelFormat != .invalid,
              source.textureType == .type2D, dest.textureType == .type2D,
              source.sampleCount == 1, dest.sampleCount == 1,
              let blit = command.makeBlitCommandEncoder()
        else { return }
        for rect in rects {
            let (x, y, w, h) = rect
            guard x >= 0, y >= 0, w > 0, h > 0,
                  x + w <= source.width, y + h <= source.height,
                  x + w <= dest.width, y + h <= dest.height
            else { continue }
            blit.copy(
                from: source, sourceSlice: 0, sourceLevel: 0,
                sourceOrigin: MTLOrigin(x: x, y: y, z: 0),
                sourceSize: MTLSize(width: w, height: h, depth: 1),
                to: dest, destinationSlice: 0, destinationLevel: 0,
                destinationOrigin: MTLOrigin(x: x, y: y, z: 0))
        }
        blit.endEncoding()
    }
}
