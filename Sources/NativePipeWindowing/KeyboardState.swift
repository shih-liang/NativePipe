import AppKit

/// A press has exactly one owner: the guest keyboard or the host input method.
/// Only the former gets raw releases and guest-side repeat; the latter must
/// continue receiving AppKit's repeated keyDown events to produce more text.
struct KeyboardState {
    struct Key: Equatable {
        let code: UInt16
        let pressed: Bool
        let flags: NSEvent.ModifierFlags
    }

    private struct Press {
        let source: UInt16
        let code: UInt16
        let sourceFlags: NSEvent.ModifierFlags
        let targetFlags: NSEvent.ModifierFlags?
    }

    // A release is translated using its press, never the current preferences.
    // Order matters: the most recent key owns the keyboard's global modifiers.
    private var rawPresses: [Press] = []
    private var textPresses: Set<UInt16> = []
    private var modifierPresses: Set<UInt16> = []
    private var guestModifiers: Set<UInt16> = []
    private var flags: NSEvent.ModifierFlags = []

    func ownsPress(_ code: UInt16) -> Bool { rawPresses.contains { $0.source == code } }
    func ownsTextPress(_ code: UInt16) -> Bool { textPresses.contains(code) }

    func shouldInterpret(_ event: NSEvent, textInputEnabled: Bool) -> Bool {
        textInputEnabled
            && event.modifierFlags.intersection([.command, .control]).isEmpty
            && !ownsPress(event.keyCode)
            && (!event.isARepeat || textPresses.contains(event.keyCode))
    }

    mutating func textHandled(_ event: NSEvent) {
        flags = event.modifierFlags
        textPresses.insert(event.keyCode)
    }

    mutating func press(
        _ event: NSEvent, mappedCode: UInt16? = nil,
        mappedFlags: NSEvent.ModifierFlags? = nil
    ) -> [Key] {
        guard !event.isARepeat, KeyTranslation.evdevCode(for: event.keyCode) != nil,
              !textPresses.contains(event.keyCode),
              !ownsPress(event.keyCode) else { return [] }
        flags = event.modifierFlags
        let code = mappedCode ?? event.keyCode
        let alreadyDown = rawPresses.contains { $0.code == code }
        rawPresses.append(.init(source: event.keyCode, code: code,
                                sourceFlags: flags.intersection(Self.chordFlags), targetFlags: mappedFlags))
        var keys = synchronizeModifiers()
        if !alreadyDown { keys.append(Key(code: code, pressed: true, flags: effectiveFlags)) }
        return keys
    }

    mutating func release(_ event: NSEvent) -> [Key] {
        flags = event.modifierFlags
        textPresses.remove(event.keyCode)
        guard let index = rawPresses.firstIndex(where: { $0.source == event.keyCode }) else { return [] }
        let releaseFlags = effectiveFlags
        let press = rawPresses.remove(at: index)
        var keys: [Key] = []
        if !rawPresses.contains(where: { $0.code == press.code }) {
            keys.append(Key(code: press.code, pressed: false, flags: releaseFlags))
        }
        keys += synchronizeModifiers()
        return keys
    }

    mutating func modifiersChanged(_ event: NSEvent) -> [Key] {
        flags = event.modifierFlags
        let wasPressed = modifierPresses.contains(event.keyCode)
        guard let pressed = KeyTranslation.modifierIsPressed(
            event.keyCode, flags: flags, wasPressed: wasPressed) else { return [] }
        if pressed { modifierPresses.insert(event.keyCode) }
        else { modifierPresses.remove(event.keyCode) }
        return synchronizeModifiers()
    }

    private static let chordFlags: NSEvent.ModifierFlags = [.shift, .control, .option, .command]
    private static let modifierKeys: [(NSEvent.ModifierFlags, [UInt16])] = [
        (.shift, [0x38, 0x3c]), (.control, [0x3b, 0x3e]),
        (.option, [0x3a, 0x3d]), (.command, [0x37, 0x36]), (.capsLock, [0x39]),
    ]

    private var effectiveFlags: NSEvent.ModifierFlags {
        var value = flags.intersection(Self.chordFlags.union(.capsLock))
        if let press = rawPresses.last, let target = press.targetFlags {
            value.subtract(press.sourceFlags)
            value.formUnion(target)
        }
        return value
    }

    /// Reconcile actual modifier edges as well as the per-event mask. Reusing a
    /// physical Control press prevents releasing it when a mapped shortcut ends.
    private mutating func synchronizeModifiers() -> [Key] {
        let desiredFlags = effectiveFlags
        var desired: Set<UInt16> = []
        for (flag, codes) in Self.modifierKeys where desiredFlags.contains(flag) {
            let physical = modifierPresses.intersection(codes)
            if physical.isEmpty { desired.insert(codes[0]) }
            else { desired.formUnion(physical) }
        }
        var result: [Key] = []
        for code in guestModifiers.subtracting(desired).sorted() {
            guestModifiers.remove(code)
            result.append(Key(code: code, pressed: false, flags: guestModifierFlags))
        }
        for code in desired.subtracting(guestModifiers).sorted() {
            guestModifiers.insert(code)
            result.append(Key(code: code, pressed: true, flags: guestModifierFlags))
        }
        return result
    }

    private var guestModifierFlags: NSEvent.ModifierFlags {
        var result: NSEvent.ModifierFlags = []
        for (flag, codes) in Self.modifierKeys where !guestModifiers.isDisjoint(with: codes) {
            result.insert(flag)
        }
        return result
    }

    mutating func releaseAll() -> [Key] {
        // Caps Lock is a persistent lock, not a modifier to clear on defocus.
        let releasedFlags = flags.intersection(.capsLock)
        let releases = (Set(rawPresses.map(\.code)).sorted() + guestModifiers.sorted()).map {
            Key(code: $0, pressed: false, flags: releasedFlags)
        }
        rawPresses.removeAll(keepingCapacity: true)
        textPresses.removeAll(keepingCapacity: true)
        modifierPresses.removeAll(keepingCapacity: true)
        guestModifiers.removeAll(keepingCapacity: true)
        flags = releasedFlags
        return releases
    }
}
