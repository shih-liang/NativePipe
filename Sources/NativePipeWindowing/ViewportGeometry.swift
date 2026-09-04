import CoreGraphics
import NativePipeProtocol

/// Which physical AppKit edges stay fixed while client-committed Wayland
/// geometry replaces the provisional frame used during an interactive resize.
struct WindowFrameAnchor: Equatable {
    enum Horizontal: Equatable { case left, right }
    enum Vertical: Equatable { case bottom, top }

    var horizontal: Horizontal
    var vertical: Vertical

    static let topLeft = WindowFrameAnchor(horizontal: .left, vertical: .top)

    static func inferred(from start: CGRect, to end: CGRect) -> Self {
        let movedLeft = abs(end.minX - start.minX)
        let movedRight = abs(end.maxX - start.maxX)
        let movedBottom = abs(end.minY - start.minY)
        let movedTop = abs(end.maxY - start.maxY)
        return WindowFrameAnchor(
            horizontal: movedLeft > movedRight ? .right : .left,
            vertical: movedBottom >= movedTop ? .top : .bottom)
    }

    func frame(size: CGSize, relativeTo current: CGRect) -> CGRect {
        CGRect(
            x: horizontal == .left ? current.minX : current.maxX - size.width,
            y: vertical == .bottom ? current.minY : current.maxY - size.height,
            width: size.width,
            height: size.height)
    }
}

/// Core Animation positions a zero-anchor child in its flipped superlayer from
/// the lower edge. Moving it by the unused height keeps the child's visual
/// top-left at the view's visual top-left without changing its committed size.
struct SurfaceLayerPlacement {
    static func topLeftPosition(container: CGSize, child: CGSize) -> CGPoint {
        CGPoint(x: 0, y: container.height - child.height)
    }
}

/// The single transform at the AppKit/Wayland window boundary.
///
/// Wayland describes the complete surface tree in surface-local logical units,
/// while the AppKit content view represents only xdg_surface.window_geometry.
/// Buffer scale and wp_viewport do not participate in this transform: they map
/// pixels into logical surface coordinates before the window boundary.
struct SurfaceCoordinateSpace: Equatable {
    /// The host boundary deliberately retains only the xdg window-geometry
    /// size. Its origin belongs to the guest surface tree; keeping it out of
    /// this type makes a second host-side origin translation impossible.
    private let logicalWindowSize: CGSize

    init(_ geometry: Windowing.Rect) {
        logicalWindowSize = CGSize(
            width: geometry.width, height: geometry.height)
    }

    /// Logical bounds of the committed scene. The host does not fit these
    /// bounds to a newer AppKit size: it waits for an exact client redraw.
    /// The guest compositor has already cropped the complete surface tree to
    /// xdg_surface.window_geometry. The host scene therefore always starts at
    /// zero. The guest compositor alone retains the geometry origin for the
    /// later translation into root wl_surface coordinates.
    var sceneBounds: CGRect {
        CGRect(origin: .zero, size: logicalWindowSize)
    }

    /// Convert AppKit content points to the zero-origin xdg window-geometry
    /// coordinate space carried by the host protocol. The guest compositor is
    /// the sole owner of the later window-geometry -> root wl_surface offset.
    func windowPoint(fromContent point: CGPoint) -> CGPoint {
        point
    }

    func contentPoint(fromWindow point: CGPoint) -> CGPoint {
        point
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
