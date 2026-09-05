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

    private var rawPresses: Set<UInt16> = []
    private var textPresses: Set<UInt16> = []
    private var modifierPresses: Set<UInt16> = []
    private var flags: NSEvent.ModifierFlags = []

    func shouldInterpret(_ event: NSEvent, textInputEnabled: Bool) -> Bool {
        textInputEnabled
            && event.modifierFlags.intersection([.command, .control]).isEmpty
            && !rawPresses.contains(event.keyCode)
            && (!event.isARepeat || textPresses.contains(event.keyCode))
    }

    mutating func textHandled(_ event: NSEvent) {
        flags = event.modifierFlags
        textPresses.insert(event.keyCode)
    }

    mutating func press(_ event: NSEvent) -> Key? {
        guard !event.isARepeat, KeyTranslation.evdevCode(for: event.keyCode) != nil,
              !textPresses.contains(event.keyCode),
              rawPresses.insert(event.keyCode).inserted else { return nil }
        flags = event.modifierFlags
        return Key(code: event.keyCode, pressed: true, flags: flags)
    }

    mutating func release(_ event: NSEvent) -> Key? {
        flags = event.modifierFlags
        textPresses.remove(event.keyCode)
        guard rawPresses.remove(event.keyCode) != nil else { return nil }
        return Key(code: event.keyCode, pressed: false, flags: flags)
    }

    mutating func modifiersChanged(_ event: NSEvent) -> Key? {
        let previousFlags = flags
        flags = event.modifierFlags
        let wasPressed = modifierPresses.contains(event.keyCode)
        guard let flag = KeyTranslation.modifierKeyCode(for: event.keyCode),
              let pressed = KeyTranslation.modifierIsPressed(
            event.keyCode, flags: flags, wasPressed: wasPressed),
            pressed != wasPressed || previousFlags.contains(flag) != flags.contains(flag) else { return nil }
        if pressed { modifierPresses.insert(event.keyCode) }
        else { modifierPresses.remove(event.keyCode) }
        return Key(code: event.keyCode, pressed: pressed, flags: flags)
    }

    mutating func releaseAll() -> [Key] {
        // Caps Lock is a persistent lock, not a modifier to clear on defocus.
        let releasedFlags = flags.intersection(.capsLock)
        let releases = (rawPresses.sorted() + modifierPresses.sorted()).map {
            Key(code: $0, pressed: false, flags: releasedFlags)
        }
        rawPresses.removeAll(keepingCapacity: true)
        textPresses.removeAll(keepingCapacity: true)
        modifierPresses.removeAll(keepingCapacity: true)
        flags = releasedFlags
        return releases
    }
}
