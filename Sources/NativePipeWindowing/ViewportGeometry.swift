import CoreGraphics
import NativePipeProtocol

/// The single transform at the AppKit/Wayland window boundary.
///
/// Wayland describes the complete surface tree in surface-local logical units,
/// while the AppKit content view represents only xdg_surface.window_geometry.
/// Buffer scale and wp_viewport do not participate in this transform: they map
/// pixels into logical surface coordinates before the window boundary.
struct SurfaceCoordinateSpace: Equatable {
    let windowGeometry: CGRect
    let contentSize: CGSize

    init(_ geometry: Windowing.Rect, contentSize: CGSize? = nil) {
        windowGeometry = CGRect(
            x: geometry.x, y: geometry.y,
            width: geometry.width, height: geometry.height)
        self.contentSize = contentSize ?? windowGeometry.size
    }

    /// CALayer bounds for the scene viewport. Setting its frame to the AppKit
    /// content bounds maps the entire committed tree as one unit while a client
    /// is still catching up with a live resize.
    /// The guest compositor has already cropped the complete surface tree to
    /// xdg_surface.window_geometry. The host scene therefore always starts at
    /// zero; the geometry origin remains only for translating input back into
    /// the root wl_surface coordinate space.
    var sceneBounds: CGRect {
        CGRect(origin: .zero, size: windowGeometry.size)
    }

    var sceneScale: CGSize {
        CGSize(
            width: windowGeometry.width > 0 ? contentSize.width / windowGeometry.width : 1,
            height: windowGeometry.height > 0 ? contentSize.height / windowGeometry.height : 1)
    }

    private var surfaceUnitsPerContentPoint: CGSize {
        CGSize(
            width: contentSize.width > 0 ? windowGeometry.width / contentSize.width : 1,
            height: contentSize.height > 0 ? windowGeometry.height / contentSize.height : 1)
    }

    func surfacePoint(fromContent point: CGPoint) -> CGPoint {
        let scale = surfaceUnitsPerContentPoint
        return CGPoint(
            x: windowGeometry.minX + point.x * scale.width,
            y: windowGeometry.minY + point.y * scale.height)
    }

    func contentPoint(fromSurface point: CGPoint) -> CGPoint {
        let scale = surfaceUnitsPerContentPoint
        return CGPoint(
            x: scale.width != 0 ? (point.x - windowGeometry.minX) / scale.width : 0,
            y: scale.height != 0 ? (point.y - windowGeometry.minY) / scale.height : 0)
    }
}

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

    /// CoreAnimation crop for an active frame stored at the top-left of a
    /// larger capacity allocation.
    func contentsRect(for logical: CGRect, allocationSize: CGSize) -> CGRect {
        let crop = bufferPixelRect(for: logical).intersection(
            CGRect(x: 0, y: 0, width: width, height: height))
        guard !crop.isNull, !crop.isEmpty,
              allocationSize.width > 0, allocationSize.height > 0
        else { return CGRect(x: 0, y: 0, width: 1, height: 1) }
        return CGRect(
            x: crop.minX / allocationSize.width,
            y: crop.minY / allocationSize.height,
            width: crop.width / allocationSize.width,
            height: crop.height / allocationSize.height)
    }

    func pixelDensity(for logical: CGRect) -> CGFloat {
        let pixels = bufferPixelRect(for: logical)
        guard logical.width > 0, logical.height > 0 else { return 1 }
        return max(max(
            pixels.width / logical.width,
            pixels.height / logical.height), 1)
    }
}
