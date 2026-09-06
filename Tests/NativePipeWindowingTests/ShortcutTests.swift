import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class ShortcutTests: XCTestCase {
    private func event(_ type: NSEvent.EventType, code: UInt16 = 0x08,
                       flags: NSEvent.ModifierFlags = .command, characters: String = "c",
                       repeated: Bool = false, window: NSWindow? = nil) -> NSEvent {
        NSEvent.keyEvent(with: type, location: .zero, modifierFlags: flags,
                        timestamp: ProcessInfo.processInfo.systemUptime,
                        windowNumber: window?.windowNumber ?? 0, context: nil,
                        characters: characters, charactersIgnoringModifiers: characters,
                        isARepeat: repeated, keyCode: code)!
    }

    func testRemappedPressAndBothReleaseOrdersHaveNoStuckKeys() {
        for commandFirst in [true, false] {
            var state = KeyboardState()
            var output = state.modifiersChanged(event(.flagsChanged, code: 0x37))
            output += state.press(event(.keyDown), mappedCode: 0x09, mappedFlags: .control)
            XCTAssertEqual(output.last, .init(code: 0x09, pressed: true, flags: .control))
            if commandFirst { output += state.modifiersChanged(event(.flagsChanged, code: 0x37, flags: [])) }
            output += state.release(event(.keyUp, flags: commandFirst ? [] : .command))
            if !commandFirst { output += state.modifiersChanged(event(.flagsChanged, code: 0x37, flags: [])) }
            XCTAssertTrue(state.releaseAll().isEmpty)
            var held: Set<UInt16> = []
            for key in output {
                if key.pressed { XCTAssertTrue(held.insert(key.code).inserted) }
                else { XCTAssertNotNil(held.remove(key.code)) }
            }
            XCTAssertTrue(held.isEmpty)
        }
    }

    func testPhysicalControlIsNotReleasedWhenMappedKeyEnds() {
        var state = KeyboardState()
        _ = state.modifiersChanged(event(.flagsChanged, code: 0x3b, flags: .control))
        _ = state.modifiersChanged(event(.flagsChanged, code: 0x37, flags: [.command, .control]))
        _ = state.press(event(.keyDown, flags: [.command, .control]), mappedCode: 0x08, mappedFlags: .control)
        let releases = state.release(event(.keyUp, flags: [.command, .control]))
        XCTAssertFalse(releases.contains { $0.code == 0x3b && !$0.pressed })
        XCTAssertTrue(state.releaseAll().contains { $0.code == 0x3b && !$0.pressed })
    }

    func testMappedAndUnmappedRolloverUsesNewestKeysModifiers() {
        var state = KeyboardState()
        _ = state.press(event(.keyDown), mappedCode: 0x08, mappedFlags: .control)
        let unhandled = state.press(event(.keyDown, code: 0x2d, characters: "n"))
        XCTAssertEqual(unhandled.last, .init(code: 0x2d, pressed: true, flags: .command))
        let restore = state.release(event(.keyUp, code: 0x2d, characters: "n"))
        XCTAssertEqual(restore.last, .init(code: 0x3b, pressed: true, flags: .control))
        _ = state.release(event(.keyUp, flags: []))
        XCTAssertTrue(state.releaseAll().isEmpty)
    }

    func testTwoSourceKeysSharingTargetHaveOnlyOneGuestPress() {
        var state = KeyboardState()
        var output = state.press(event(.keyDown), mappedCode: 0x09, mappedFlags: .control)
        output += state.press(event(.keyDown, code: 0x07, characters: "x"), mappedCode: 0x09, mappedFlags: .control)
        output += state.release(event(.keyUp))
        output += state.release(event(.keyUp, code: 0x07, flags: [], characters: "x"))
        XCTAssertEqual(output.filter { $0.code == 0x09 }.map(\.pressed), [true, false])
        XCTAssertTrue(state.releaseAll().isEmpty)
    }

    func testSymbolicSourceDoesNotAssumeUSPhysicalPosition() throws {
        let germanZ = event(.keyDown, code: 0x10, characters: "z")
        XCTAssertEqual(ShortcutTranslation.chord(for: germanZ), .init(.z, .logo))
        XCTAssertEqual(ShortcutTranslation.keyCode(for: .z, source: germanZ), 0x10)
        XCTAssertEqual(ShortcutTranslation.keyCode(for: .home, source: germanZ), 0x73)
        let shifted = event(.keyDown, flags: [.command, .shift, .capsLock], characters: "C")
        XCTAssertEqual(ShortcutTranslation.chord(for: shifted), .init(.c, [.logo, .shift]))
    }

    private final class Output {
        var commands: [Windowing.HostCommand] = []
        var presses: [(UInt32, Windowing.Modifiers)] {
            commands.compactMap {
                if case .key(_, let code, true, let flags) = $0 { return (code, flags) }
                return nil
            }
        }
    }

    private func fixture() throws -> (WindowBridge, NativeWindow, NSView, Output) {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        bridge.apply(.surfaceCreated(surface: 1))
        bridge.apply(.toplevelCreated(window: 1, surface: 1))
        let native = try XCTUnwrap(bridge.window(1))
        native.revealToplevel(width: 160, height: 100)
        let window = try XCTUnwrap(native.window)
        let view = try XCTUnwrap(window.contentView)
        window.makeFirstResponder(view)
        let output = Output()
        bridge.output = { output.commands.append($0) }
        return (bridge, native, view, output)
    }

    func testQuitEquivalentIsConsumedWithDefaultDisabledAndDeletedRules() throws {
        let (bridge, native, view, output) = try fixture()
        defer { bridge.closeAll() }
        guard native.window?.firstResponder === view else { throw XCTSkip("Needs WindowServer") }
        for preferences in [KeyboardShortcutPreferences(), .init(enabled: false), .init(rules: [])] {
            bridge.setIntegrationPreferences(.init(shortcuts: preferences))
            let down = event(.keyDown, code: 0x0c, characters: "q", window: native.window)
            XCTAssertTrue(view.performKeyEquivalent(with: down))
            let expected: Windowing.Modifiers = preferences.enabled && !preferences.rules.isEmpty ? .control : .logo
            XCTAssertTrue(output.presses.contains { $0.0 == 16 && $0.1 == expected })
            view.keyUp(with: event(.keyUp, code: 0x0c, flags: [], characters: "q", window: native.window))
            native.releasePressedKeys()
            output.commands.removeAll()
        }
        bridge.setSuspended(true)
        XCTAssertTrue(view.performKeyEquivalent(with: event(.keyDown, code: 0x0c, characters: "q")))
        XCTAssertTrue(output.commands.isEmpty)
    }

    func testApplicationOverrideAndLiveRuleEditUseSameWindow() throws {
        let (bridge, _, view, output) = try fixture()
        defer { bridge.closeAll() }
        bridge.apply(.appIDChanged(window: 1, appID: "qterminal"))
        view.keyDown(with: event(.keyDown))
        XCTAssertTrue(output.presses.contains { $0.0 == 46 && $0.1 == [.control, .shift] })
        var preferences = KeyboardShortcutPreferences()
        let index = preferences.rules.firstIndex { $0.applicationIDs?.contains("qterminal") == true && $0.source.key == .c }!
        preferences.rules[index].target = .init(.v, .alt)
        bridge.setIntegrationPreferences(.init(shortcuts: preferences))
        view.keyUp(with: event(.keyUp, flags: []))
        XCTAssertTrue(output.commands.contains {
            if case .key(_, 46, false, _) = $0 { return true }; return false
        })
        output.commands.removeAll()
        view.keyDown(with: event(.keyDown))
        XCTAssertTrue(output.presses.contains { $0.0 == 47 && $0.1 == .alt })
        view.keyUp(with: event(.keyUp, flags: []))
    }

    func testMappedKeyNeverStartsIMEOrDuplicatesHostRepeat() throws {
        let (bridge, native, view, output) = try fixture()
        defer { bridge.closeAll() }
        native.setTextInput(enabled: true)
        let rule = KeyboardShortcutRule(id: 1, source: .init(.c), target: .init(.v, .control))
        bridge.setIntegrationPreferences(.init(shortcuts: .init(rules: [rule])))
        view.keyDown(with: event(.keyDown, flags: []))
        for _ in 0..<10 { view.keyDown(with: event(.keyDown, flags: [], repeated: true)) }
        view.keyUp(with: event(.keyUp, flags: []))
        XCTAssertEqual(output.presses.filter { $0.0 == 47 }.count, 1)
        XCTAssertFalse(output.commands.contains {
            switch $0 { case .textCommit, .textPreedit: return true; default: return false }
        })
    }

    func testInstallingRuleDoesNotTakeOverAnIMEOwnedRepeat() throws {
        let (bridge, native, view, output) = try fixture()
        defer { bridge.closeAll() }
        native.setTextInput(enabled: true)
        let down = event(.keyDown, flags: [], window: native.window)
        guard down.window === native.window else { throw XCTSkip("Needs WindowServer") }
        view.keyDown(with: down)
        let rule = KeyboardShortcutRule(id: 1, source: .init(.c), target: .init(.v, .control))
        bridge.setIntegrationPreferences(.init(shortcuts: .init(rules: [rule])))
        output.commands.removeAll()
        view.keyDown(with: event(.keyDown, flags: [], repeated: true, window: native.window))
        XCTAssertTrue(output.presses.isEmpty)
        XCTAssertTrue(output.commands.contains {
            switch $0 { case .textCommit, .textPreedit: return true; default: return false }
        })
        view.keyUp(with: event(.keyUp, flags: [], window: native.window))
        view.keyDown(with: down)
        XCTAssertTrue(output.presses.contains { $0.0 == 47 && $0.1 == .control })
    }

    private final class QuitTarget: NSObject {
        var calls = 0
        @objc func quit(_ sender: Any?) { calls += 1 }
    }

    func testAppKitDispatchDoesNotInvokeHostQuitMenu() throws {
        let (bridge, native, view, output) = try fixture()
        let previousMenu = NSApp.mainMenu
        defer { NSApp.mainMenu = previousMenu; bridge.closeAll() }
        let window = try XCTUnwrap(native.window)
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(view)
        guard window.isKeyWindow, window.firstResponder === view else { throw XCTSkip("Needs a key AppKit window") }
        let target = QuitTarget()
        let menu = NSMenu()
        let item = menu.addItem(withTitle: "Quit VMHost", action: #selector(QuitTarget.quit(_:)), keyEquivalent: "q")
        item.target = target
        NSApp.mainMenu = menu
        for enabled in [true, false] {
            bridge.setIntegrationPreferences(.init(shortcuts: .init(enabled: enabled)))
            NSApp.sendEvent(event(.keyDown, code: 0x0c, characters: "q", window: window))
            NSApp.sendEvent(event(.keyUp, code: 0x0c, flags: [], characters: "q", window: window))
            XCTAssertEqual(target.calls, 0)
            XCTAssertTrue(output.presses.contains { $0.0 == 16 && $0.1 == (enabled ? .control : .logo) })
            native.releasePressedKeys()
            output.commands.removeAll()
        }
    }
}
