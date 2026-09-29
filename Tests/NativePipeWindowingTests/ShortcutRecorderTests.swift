import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class ShortcutRecorderTests: XCTestCase {
    private func event(_ type: NSEvent.EventType, code: UInt16 = 40,
                       flags: NSEvent.ModifierFlags = [.command, .control, .option, .shift],
                       characters: String = "K", repeated: Bool = false) -> NSEvent {
        NSEvent.keyEvent(with: type, location: .zero, modifierFlags: flags, timestamp: 0,
            windowNumber: 0, context: nil, characters: characters, charactersIgnoringModifiers: characters,
            isARepeat: repeated, keyCode: code)!
    }

    private func fixture() throws -> (NSWindow, ShortcutRecorderButton) {
        _ = NSApplication.shared
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 300, height: 100),
            styleMask: .titled, backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        let button = ShortcutRecorderButton()
        window.contentView?.addSubview(button)
        button.beginRecording()
        guard button.isRecording else { window.close(); throw XCTSkip("Requires an AppKit window") }
        return (window, button)
    }

    func testRecordingCommitsOnlyOnReleaseAndEscapeCancels() throws {
        let (window, button) = try fixture()
        defer { button.finishRecording(); window.close() }
        var changes: [ShortcutChord] = []
        button.onChange = { changes.append($0); button.chord = $0; return true }
        button.record(event(.flagsChanged))
        button.record(event(.keyDown, repeated: true))
        XCTAssertTrue(changes.isEmpty)
        button.record(event(.keyDown))
        XCTAssertTrue(changes.isEmpty, "Do not activate a global binding on its recording key-down")
        // Releasing modifiers first must preserve the modifiers at key-down.
        button.record(event(.keyUp, flags: []))
        XCTAssertEqual(changes, [.init(.k, [.logo, .control, .alt, .shift])])
        XCTAssertFalse(button.isRecording)
        button.beginRecording()
        button.record(event(.keyDown, code: 53, flags: [], characters: "\u{1b}"))
        XCTAssertFalse(button.isRecording)
        XCTAssertEqual(changes.count, 1)
    }

    func testRejectedChordAndFocusLossLeaveOriginalUnchanged() throws {
        let (window, button) = try fixture()
        defer { button.finishRecording(); window.close() }
        let original = button.chord
        button.onChange = { _ in false }
        button.record(event(.keyDown)); button.record(event(.keyUp))
        XCTAssertTrue(button.isRecording)
        XCTAssertEqual(button.chord, original)
        window.makeFirstResponder(nil)
        XCTAssertFalse(button.isRecording)
        button.beginRecording(); button.removeFromSuperview()
        XCTAssertFalse(button.isRecording)
        XCTAssertEqual(button.chord, original)
    }

    func testAllModifierCombinationsUseExistingChordModel() throws {
        for mask in UInt32(0)..<16 {
            let modifiers = Windowing.Modifiers(rawValue: mask)
            let translated = ShortcutTranslation.chord(for: event(.keyDown,
                flags: ShortcutTranslation.flags(from: modifiers), characters: "k"))
            XCTAssertEqual(translated, .init(.k, modifiers))
        }
        for key in ShortcutKey.allCases where key.isFunctionKey {
            let code = try XCTUnwrap(ShortcutTranslation.keyCode(for: key))
            XCTAssertEqual(ShortcutTranslation.chord(for: event(.keyDown, code: code, flags: [.function], characters: "")), .init(key))
            XCTAssertNotNil(KeyTranslation.evdevCode(for: code), "Shared editor keys must also work in Linux shortcut mappings")
        }
    }
}
