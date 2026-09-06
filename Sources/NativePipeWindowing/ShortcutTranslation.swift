import AppKit
import Carbon
import NativePipeProtocol

enum ShortcutTranslation {
    private static let namedKeys: [UInt16: ShortcutKey] = [
        0x24: .enter, 0x30: .tab, 0x31: .space, 0x33: .backspace, 0x35: .escape,
        0x75: .delete, 0x73: .home, 0x77: .end, 0x74: .pageUp, 0x79: .pageDown,
        0x7b: .left, 0x7c: .right, 0x7d: .down, 0x7e: .up,
        0x7a: .f1, 0x78: .f2, 0x63: .f3, 0x76: .f4, 0x60: .f5, 0x61: .f6,
        0x62: .f7, 0x64: .f8, 0x65: .f9, 0x6d: .f10, 0x67: .f11, 0x6f: .f12,
    ]

    static func chord(for event: NSEvent) -> ShortcutChord? {
        let key = namedKeys[event.keyCode]
            ?? event.charactersIgnoringModifiers.flatMap { ShortcutKey(rawValue: $0.lowercased()) }
            ?? layoutKey(for: event.keyCode)
        guard let key else { return nil }
        return .init(key, KeyTranslation.modifiers(from: event.modifierFlags).intersection(
            [.shift, .control, .alt, .logo]))
    }

    static func keyCode(for key: ShortcutKey, source event: NSEvent) -> UInt16? {
        // Modifier-only changes preserve the actual source position, including
        // non-US layouts and Command-specific layouts such as Dvorak-QWERTY.
        if chord(for: event)?.key == key { return event.keyCode }
        if let named = namedKeys.first(where: { $0.value == key }) { return named.key }
        return (UInt16(0)...UInt16(0x7f)).first {
            KeyTranslation.evdevCode(for: $0) != nil && layoutKey(for: $0) == key
        }
    }

    private static func layoutKey(for code: UInt16) -> ShortcutKey? {
        guard let source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource()?.takeRetainedValue(),
              let pointer = TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData)
        else { return nil }
        let data = Unmanaged<CFData>.fromOpaque(pointer).takeUnretainedValue()
        guard let bytes = CFDataGetBytePtr(data) else { return nil }
        let layout = UnsafeRawPointer(bytes).assumingMemoryBound(to: UCKeyboardLayout.self)
        var deadKeyState: UInt32 = 0
        var length = 0
        var characters = [UniChar](repeating: 0, count: 8)
        let status = UCKeyTranslate(layout, code, UInt16(kUCKeyActionDown), 0,
                                    UInt32(LMGetKbdType()), OptionBits(kUCKeyTranslateNoDeadKeysMask),
                                    &deadKeyState, characters.count, &length, &characters)
        guard status == noErr, length > 0 else { return nil }
        return ShortcutKey(rawValue: String(utf16CodeUnits: characters, count: length).lowercased())
    }

    static func flags(from modifiers: Windowing.Modifiers) -> NSEvent.ModifierFlags {
        var flags: NSEvent.ModifierFlags = []
        if modifiers.contains(.shift) { flags.insert(.shift) }
        if modifiers.contains(.control) { flags.insert(.control) }
        if modifiers.contains(.alt) { flags.insert(.option) }
        if modifiers.contains(.logo) { flags.insert(.command) }
        if modifiers.contains(.capsLock) { flags.insert(.capsLock) }
        return flags
    }
}
