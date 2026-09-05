import AppKit
import IOKit.hidsystem
import NativePipeProtocol

/// macOS virtual key codes to Linux evdev codes.
///
/// Wayland's `wl_keyboard.key` carries an evdev code; the client adds 8 to get
/// an xkb keycode. There is no arithmetic relationship between the two
/// numbering schemes — macOS numbers keys by their position in the original
/// Apple Extended Keyboard's scan order — so this is a table and has to be one.
///
/// Keys absent here produce nothing. That is deliberate: sending a wrong code is
/// worse than sending none, because the client acts on it.
enum KeyTranslation {
    /// Physical keys, by macOS `kVK_*` value.
    private static let evdev: [UInt16: UInt32] = [
        // Letters
        0x00: 30, 0x0B: 48, 0x08: 46, 0x02: 32, 0x0E: 18, 0x03: 33, 0x05: 34,
        0x04: 35, 0x22: 23, 0x26: 36, 0x28: 37, 0x25: 38, 0x2E: 50, 0x2D: 49,
        0x1F: 24, 0x23: 25, 0x0C: 16, 0x0F: 19, 0x01: 31, 0x11: 20, 0x20: 22,
        0x09: 47, 0x0D: 17, 0x07: 45, 0x10: 21, 0x06: 44,

        // Digits
        0x1D: 11, 0x12: 2, 0x13: 3, 0x14: 4, 0x15: 5, 0x17: 6,
        0x16: 7, 0x1A: 8, 0x1C: 9, 0x19: 10,

        // Punctuation
        0x1B: 12,  // minus
        0x18: 13,  // equal
        0x21: 26,  // [
        0x1E: 27,  // ]
        0x2A: 43,  // backslash
        0x29: 39,  // ;
        0x27: 40,  // '
        0x32: 41,  // `
        0x2B: 51,  // ,
        0x2F: 52,  // .
        0x2C: 53,  // /

        // Editing and navigation
        0x24: 28,   // return
        0x30: 15,   // tab
        0x31: 57,   // space
        0x33: 14,   // backspace
        0x35: 1,    // escape
        0x75: 111,  // forward delete
        0x73: 102,  // home
        0x77: 107,  // end
        0x74: 104,  // page up
        0x79: 109,  // page down
        0x7B: 105, 0x7C: 106, 0x7D: 108, 0x7E: 103,  // arrows

        // Modifiers. Option is Alt and Command is Super — same physical keys,
        // different names on the two platforms.
        0x38: 42, 0x3C: 54,    // shift
        0x3B: 29, 0x3E: 97,    // control
        0x3A: 56, 0x3D: 100,   // option / alt
        0x37: 125, 0x36: 126,  // command / super
        0x39: 58,              // caps lock

        // Function keys
        0x7A: 59, 0x78: 60, 0x63: 61, 0x76: 62, 0x60: 63, 0x61: 64,
        0x62: 65, 0x64: 66, 0x65: 67, 0x6D: 68, 0x67: 87, 0x6F: 88,
    ]

    static func evdevCode(for macKeyCode: UInt16, swapCommandAndControl: Bool = false) -> UInt32? {
        if swapCommandAndControl {
            switch macKeyCode {
            case 0x37: return 29
            case 0x36: return 97
            case 0x3B: return 125
            case 0x3E: return 126
            default: break
            }
        }
        return evdev[macKeyCode]
    }

    /// Modifier keys report state through `flagsChanged` rather than key events,
    /// so the pressed/released edge has to be inferred from the flags.
    static func modifierKeyCode(for macKeyCode: UInt16) -> NSEvent.ModifierFlags? {
        switch macKeyCode {
        case 0x38, 0x3C: return .shift
        case 0x3B, 0x3E: return .control
        case 0x3A, 0x3D: return .option
        case 0x37, 0x36: return .command
        case 0x39: return .capsLock
        default: return nil
        }
    }

    /// The aggregate Shift/Control/etc. flag cannot distinguish releasing one
    /// side while the other remains held. AppKit also supplies device-side bits.
    static func modifierIsPressed(
        _ macKeyCode: UInt16, flags: NSEvent.ModifierFlags, wasPressed: Bool
    ) -> Bool? {
        guard let flag = modifierKeyCode(for: macKeyCode) else { return nil }
        if flag == .capsLock { return flags.contains(flag) }
        let left: Int32, right: Int32, isLeft: Bool
        switch macKeyCode {
        case 0x38, 0x3C:
            (left, right, isLeft) = (NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK, macKeyCode == 0x38)
        case 0x3B, 0x3E:
            (left, right, isLeft) = (NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK, macKeyCode == 0x3B)
        case 0x3A, 0x3D:
            (left, right, isLeft) = (NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK, macKeyCode == 0x3A)
        default:
            (left, right, isLeft) = (NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK, macKeyCode == 0x37)
        }
        if flags.rawValue & UInt(left | right) != 0 {
            return flags.rawValue & UInt(isLeft ? left : right) != 0
        }
        // Events constructed without device-side bits still identify the key
        // that changed; toggle that key, not the aggregate modifier state.
        return flags.contains(flag) && !wasPressed
    }

    static func modifiers(
        from flags: NSEvent.ModifierFlags, swapCommandAndControl: Bool = false
    ) -> Windowing.Modifiers {
        var result: Windowing.Modifiers = []
        if flags.contains(.shift) { result.insert(.shift) }
        if flags.contains(.control) { result.insert(swapCommandAndControl ? .logo : .control) }
        if flags.contains(.option) { result.insert(.alt) }
        if flags.contains(.command) { result.insert(swapCommandAndControl ? .control : .logo) }
        if flags.contains(.capsLock) { result.insert(.capsLock) }
        return result
    }
}
