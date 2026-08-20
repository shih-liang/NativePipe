import CoreGraphics
import NativePipeProtocol

extension Windowing.Frame {
    /// The crop before viewporter scales it, expressed in coordinates after
    /// wl_surface buffer scale (the coordinate system defined by the protocol).
    var effectiveViewportSource: CGRect {
        let scale = CGFloat(min(max(self.scale, 1), 4))
        let natural = CGRect(
            x: 0, y: 0,
            width: CGFloat(width) / scale,
            height: CGFloat(height) / scale)
        guard let source = viewportSource,
              source.x >= 0, source.y >= 0,
              source.width > 0, source.height > 0
        else { return natural }
        return CGRect(
            x: CGFloat(source.x), y: CGFloat(source.y),
            width: CGFloat(source.width), height: CGFloat(source.height))
    }

    /// Final wl_surface size in logical coordinates, after buffer scale,
    /// viewporter crop and destination scaling have all been applied.
    var logicalSurfaceSize: CGSize {
        if let destination = viewportDestination,
           destination.width > 0, destination.height > 0 {
            return CGSize(
                width: CGFloat(destination.width),
                height: CGFloat(destination.height))
        }
        return effectiveViewportSource.size
    }

    /// NativePipe's shared wl_surface -> AppKit size policy. Wayland logical
    /// surface units map one-to-one to AppKit points; buffer_scale and
    /// wp_viewport determine how many source pixels back each point. Windows,
    /// subsurfaces, drag icons and wl_pointer.set_cursor surfaces all use this
    /// value. Semantic cursor-shape-v1 cursors intentionally do not.
    var appKitPointSize: CGSize { logicalSurfaceSize }

    /// Maps a rectangle in final surface-local coordinates back to the source
    /// buffer. Metal and CoreAnimation consume buffer pixels, not Wayland
    /// logical coordinates.
    func bufferPixelRect(for logical: CGRect) -> CGRect {
        let source = effectiveViewportSource
        let destination = logicalSurfaceSize
        guard destination.width > 0, destination.height > 0 else { return .zero }
        let scale = CGFloat(min(max(self.scale, 1), 4))
        return CGRect(
            x: (source.minX + logical.minX * source.width / destination.width) * scale,
            y: (source.minY + logical.minY * source.height / destination.height) * scale,
            width: logical.width * source.width / destination.width * scale,
            height: logical.height * source.height / destination.height * scale)
    }

    var fullViewportBufferPixelRect: CGRect {
        bufferPixelRect(for: CGRect(origin: .zero, size: logicalSurfaceSize))
    }

    func pixelDensity(for logical: CGRect) -> CGFloat {
        let pixels = bufferPixelRect(for: logical)
        guard logical.width > 0, logical.height > 0 else { return 1 }
        return max(max(
            pixels.width / logical.width,
            pixels.height / logical.height), 1)
    }
}
