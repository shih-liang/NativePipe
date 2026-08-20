import AppKit
import NativePipeProtocol

/// Maps semantic Wayland cursors to public AppKit cursors.
///
/// There is deliberately no size input here. A cursor selected through
/// cursor-shape-v1 is native UI, so AppKit owns its point size, Retina asset and
/// accessibility scaling. wl_pointer.set_cursor surfaces take the separate
/// geometry-preserving path in WindowBridge.
@MainActor
enum NativeCursorResolver {
    static func cursor(for shape: Windowing.CursorShape) -> NSCursor {
        switch shape {
        case .defaultShape:
            return .arrow
        case .contextMenu:
            return .contextualMenu
        case .help, .progress, .wait:
            // AppKit has no public help/wait cursor. The spinning wait cursor is
            // WindowServer policy for an unresponsive application, not an
            // application-selectable NSCursor, so the native fallback is arrow.
            return .arrow
        case .pointer:
            return .pointingHand
        case .cell, .crosshair:
            return .crosshair
        case .text:
            return .iBeam
        case .verticalText:
            return .iBeamCursorForVerticalLayout
        case .alias:
            return .dragLink
        case .copy:
            return .dragCopy
        case .move, .grabbing:
            return .closedHand
        case .noDrop, .notAllowed:
            return .operationNotAllowed
        case .grab, .allScroll:
            return .openHand

        case .eResize:
            if #available(macOS 15.0, *) {
                return .frameResize(position: .right, directions: .all)
            }
            return .resizeRight
        case .nResize:
            if #available(macOS 15.0, *) {
                return .frameResize(position: .top, directions: .all)
            }
            return .resizeUp
        case .neResize:
            if #available(macOS 15.0, *) {
                return .frameResize(position: .topRight, directions: .all)
            }
            return .resizeUpDown
        case .nwResize:
            if #available(macOS 15.0, *) {
                return .frameResize(position: .topLeft, directions: .all)
            }
            return .resizeUpDown
        case .sResize:
            if #available(macOS 15.0, *) {
                return .frameResize(position: .bottom, directions: .all)
            }
            return .resizeDown
        case .seResize:
            if #available(macOS 15.0, *) {
                return .frameResize(position: .bottomRight, directions: .all)
            }
            return .resizeUpDown
        case .swResize:
            if #available(macOS 15.0, *) {
                return .frameResize(position: .bottomLeft, directions: .all)
            }
            return .resizeUpDown
        case .wResize:
            if #available(macOS 15.0, *) {
                return .frameResize(position: .left, directions: .all)
            }
            return .resizeLeft
        case .ewResize:
            if #available(macOS 15.0, *) {
                return .columnResize(directions: .all)
            }
            return .resizeLeftRight
        case .nsResize:
            if #available(macOS 15.0, *) {
                return .rowResize(directions: .all)
            }
            return .resizeUpDown
        case .neswResize:
            if #available(macOS 15.0, *) {
                return .frameResize(position: .topRight, directions: .all)
            }
            return .resizeUpDown
        case .nwseResize:
            if #available(macOS 15.0, *) {
                return .frameResize(position: .topLeft, directions: .all)
            }
            return .resizeUpDown
        case .colResize:
            if #available(macOS 15.0, *) {
                return .columnResize(directions: .all)
            }
            return .resizeLeftRight
        case .rowResize:
            if #available(macOS 15.0, *) {
                return .rowResize(directions: .all)
            }
            return .resizeUpDown
        case .zoomIn:
            if #available(macOS 15.0, *) { return .zoomIn }
            return .arrow
        case .zoomOut:
            if #available(macOS 15.0, *) { return .zoomOut }
            return .arrow
        }
    }
}
