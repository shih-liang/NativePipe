import AppKit
import IOKit.hidsystem
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class KeyboardTests: XCTestCase {
    private func event(
        _ type: NSEvent.EventType, code: UInt16 = 0x08,
        flags: NSEvent.ModifierFlags = [], repeat repeated: Bool = false,
        characters: String = "c",
        window: NSWindow? = nil
    ) -> NSEvent {
        NSEvent.keyEvent(
            with: type, location: .zero, modifierFlags: flags,
            timestamp: ProcessInfo.processInfo.systemUptime,
            windowNumber: window?.windowNumber ?? 0, context: nil,
            characters: characters, charactersIgnoringModifiers: characters,
            isARepeat: repeated, keyCode: code)!
    }

    func testRawRepeatHasExactlyOnePressAndRelease() {
        var state = KeyboardState()
        let down = event(.keyDown)
        XCTAssertFalse(state.press(down).isEmpty)
        XCTAssertTrue(state.press(down).isEmpty)
        XCTAssertTrue(state.press(event(.keyDown, repeat: true)).isEmpty)
        XCTAssertFalse(state.shouldInterpret(
            event(.keyDown, repeat: true), textInputEnabled: true))
        XCTAssertFalse(state.release(event(.keyUp)).isEmpty)
        XCTAssertTrue(state.release(event(.keyUp)).isEmpty)
        XCTAssertTrue(state.releaseAll().isEmpty)
    }

    func testIMEConsumedPressContinuesToReceiveHostRepeat() {
        var state = KeyboardState()
        let down = event(.keyDown)
        XCTAssertTrue(state.shouldInterpret(down, textInputEnabled: true))
        state.textHandled(down)
        for _ in 0..<10 {
            let repeated = event(.keyDown, repeat: true)
            XCTAssertTrue(state.shouldInterpret(repeated, textInputEnabled: true))
            state.textHandled(repeated)
            XCTAssertTrue(state.press(repeated).isEmpty)
        }
        XCTAssertTrue(state.release(event(.keyUp)).isEmpty)
        XCTAssertFalse(state.shouldInterpret(
            event(.keyDown, repeat: true), textInputEnabled: true))
        XCTAssertTrue(state.releaseAll().isEmpty)
    }

    func testShortcutBypassesIMEButOptionTextDoesNot() {
        let state = KeyboardState()
        for flags: NSEvent.ModifierFlags in [.command, .control, [.command, .shift]] {
            XCTAssertFalse(state.shouldInterpret(event(.keyDown, flags: flags), textInputEnabled: true))
        }
        XCTAssertTrue(state.shouldInterpret(event(.keyDown, flags: .option), textInputEnabled: true))
        XCTAssertFalse(state.shouldInterpret(event(.keyDown), textInputEnabled: false))
    }

    func testModifierSidesReleaseIndependently() throws {
        var state = KeyboardState()
        let left = NSEvent.ModifierFlags(rawValue: UInt(NX_DEVICELSHIFTKEYMASK))
        let right = NSEvent.ModifierFlags(rawValue: UInt(NX_DEVICERSHIFTKEYMASK))
        XCTAssertTrue(try XCTUnwrap(state.modifiersChanged(
            event(.flagsChanged, code: 0x38, flags: [.shift, left])).first).pressed)
        XCTAssertTrue(try XCTUnwrap(state.modifiersChanged(
            event(.flagsChanged, code: 0x3C, flags: [.shift, left, right])).first).pressed)
        XCTAssertFalse(try XCTUnwrap(state.modifiersChanged(
            event(.flagsChanged, code: 0x38, flags: [.shift, right])).first).pressed)
        XCTAssertFalse(try XCTUnwrap(state.modifiersChanged(
            event(.flagsChanged, code: 0x3C)).first).pressed)
        XCTAssertTrue(state.releaseAll().isEmpty)
    }

    func testResetReleasesRawAndModifierKeysButNotIMEText() {
        var state = KeyboardState()
        _ = state.modifiersChanged(event(.flagsChanged, code: 0x37, flags: .command))
        _ = state.press(event(.keyDown, flags: .command))
        state.textHandled(event(.keyDown, code: 0x00))
        let releases = state.releaseAll()
        XCTAssertEqual(releases.map(\.code), [0x08, 0x37])
        XCTAssertTrue(releases.allSatisfy { !$0.pressed && $0.flags.isEmpty })
        XCTAssertTrue(state.release(event(.keyUp)).isEmpty)
        XCTAssertFalse(state.shouldInterpret(
            event(.keyDown, code: 0x00, repeat: true), textInputEnabled: true))
        XCTAssertTrue(state.releaseAll().isEmpty)
    }

    func testModifierHeldBeforeFocusStillClearsOnRelease() throws {
        var state = KeyboardState()
        // The Shift press happened before the view had focus. Its state was
        // nevertheless advertised with C, and must not remain depressed.
        _ = state.press(event(.keyDown, flags: .shift))
        let released = try XCTUnwrap(state.modifiersChanged(event(.flagsChanged, code: 0x38)).first)
        XCTAssertFalse(released.pressed)
        XCTAssertTrue(released.flags.isEmpty)
        XCTAssertEqual(state.releaseAll().map(\.code), [0x08])
    }

    func testPhysicalModifiersAreNeverGloballySwapped() {
        XCTAssertEqual(KeyTranslation.evdevCode(for: 0x3B), 29)
        XCTAssertEqual(KeyTranslation.evdevCode(for: 0x3E), 97)
        XCTAssertEqual(KeyTranslation.modifiers(from: .control), .control)
        XCTAssertEqual(KeyTranslation.evdevCode(for: 0x37), 125)
        XCTAssertEqual(KeyTranslation.modifiers(from: .command), .logo)
    }

    private struct SentKey: Equatable {
        let code: UInt32
        let pressed: Bool
        let modifiers: Windowing.Modifiers
    }

    private final class Output {
        var commands: [Windowing.HostCommand] = []
        var keys: [SentKey] {
            commands.compactMap {
                guard case .key(_, let code, let pressed, let modifiers) = $0 else { return nil }
                return SentKey(code: code, pressed: pressed, modifiers: modifiers)
            }
        }
    }

    private func fixture() throws -> (WindowBridge, NativeWindow, NSView, Output) {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil)
        bridge.setIntegrationPreferences(.init(shortcuts: .init(enabled: false)))
        bridge.apply(.surfaceCreated(surface: 1))
        bridge.apply(.toplevelCreated(window: 1, surface: 1))
        let native = try XCTUnwrap(bridge.window(1))
        native.revealToplevel(width: 160, height: 100)
        let view = try XCTUnwrap(native.window?.contentView)
        let output = Output()
        bridge.output = { output.commands.append($0) }
        return (bridge, native, view, output)
    }

    func testCommandReleaseReachesGuestThroughAppKitMonitor() throws {
        let (bridge, native, view, output) = try fixture()
        defer { bridge.closeAll() }
        view.keyDown(with: event(.keyDown, flags: .command, window: native.window))
        let up = event(.keyUp, flags: .command, window: native.window)
        guard up.window === native.window else {
            throw XCTSkip("AppKit event dispatch requires a WindowServer-backed window")
        }
        NSApp.sendEvent(up)
        XCTAssertEqual(output.keys, [
            SentKey(code: 125, pressed: true, modifiers: .logo),
            SentKey(code: 46, pressed: true, modifiers: .logo),
            SentKey(code: 46, pressed: false, modifiers: .logo),
        ])
        view.keyUp(with: up)
        XCTAssertEqual(output.keys.count, 3, "normal dispatch must not duplicate the rescued release")
        XCTAssertFalse(bridge.handleKeyUp(event(.keyUp, code: 0x00, flags: .command, window: native.window)))
    }

    func testAppKitTextInputReceivesRepeatedLettersAndDigitsWithoutRawKeys() throws {
        let (bridge, native, view, output) = try fixture()
        defer { bridge.closeAll() }
        let down = event(.keyDown, window: native.window)
        guard down.window === native.window else {
            throw XCTSkip("AppKit text input requires a WindowServer-backed window")
        }
        native.setTextInput(enabled: true)
        for (code, characters): (UInt16, String) in [(0x08, "c"), (0x12, "1"), (0x1D, "0")] {
            for index in 0..<6 {
                output.commands.removeAll()
                view.keyDown(with: event(
                    .keyDown, code: code, repeat: index > 0,
                    characters: characters, window: native.window))
                XCTAssertTrue(output.commands.contains {
                    switch $0 {
                    case .textCommit(_, let text), .textPreedit(_, let text, _, _):
                        return !text.isEmpty
                    default: return false
                    }
                }, "\(characters) event \(index) must reach the input method, including repeats")
                XCTAssertTrue(output.keys.isEmpty, "text must not also start guest-side key repeat")
            }
            view.keyUp(with: event(.keyUp, code: code, characters: characters, window: native.window))
            XCTAssertTrue(output.keys.isEmpty, "a text-only press has no raw release")
        }
    }

    func testFocusLossReleasesKeysBeforeSendingLeave() throws {
        let (bridge, native, view, output) = try fixture()
        defer { bridge.closeAll() }
        view.flagsChanged(with: event(.flagsChanged, code: 0x37, flags: .command))
        view.keyDown(with: event(.keyDown, flags: .command))
        native.windowDidResignKey(Notification(name: NSWindow.didResignKeyNotification))
        XCTAssertEqual(output.keys.map(\.pressed), [true, true, false, false])
        let leave = try XCTUnwrap(output.commands.firstIndex {
            if case .keyboardFocus(window: nil) = $0 { return true }
            return false
        })
        XCTAssertEqual(output.commands[..<leave].filter {
            if case .key(_, _, false, _) = $0 { return true }
            return false
        }.count, 2)
        view.keyUp(with: event(.keyUp))
        XCTAssertEqual(output.keys.count, 4)
    }

    func testApplicationDeactivationReleasesOutstandingPresses() throws {
        let (bridge, _, view, output) = try fixture()
        defer { bridge.closeAll() }
        view.keyDown(with: event(.keyDown))
        NotificationCenter.default.post(name: NSApplication.didResignActiveNotification, object: NSApp)
        XCTAssertEqual(output.keys.map(\.pressed), [true, false])
        view.keyUp(with: event(.keyUp))
        XCTAssertEqual(output.keys.count, 2)
    }

    func testChangingRulesPreservesAnOutstandingPress() throws {
        let (bridge, _, view, output) = try fixture()
        defer { bridge.closeAll() }
        bridge.setIntegrationPreferences(.init())
        view.flagsChanged(with: event(.flagsChanged, code: 0x37, flags: .command))
        view.keyDown(with: event(.keyDown, flags: .command))
        let count = output.keys.count
        bridge.setIntegrationPreferences(.init(shortcuts: .init(enabled: false)))
        XCTAssertEqual(output.keys.count, count, "editing a rule must not release a held shortcut early")
        view.keyUp(with: event(.keyUp, flags: .command))
        XCTAssertTrue(output.keys.contains(SentKey(code: 46, pressed: false, modifiers: .control)))
        view.flagsChanged(with: event(.flagsChanged, code: 0x37))
        XCTAssertEqual(output.keys.last, SentKey(code: 125, pressed: false, modifiers: []))
    }

    func testSuspensionReleasesAndDoesNotQueueNewPresses() throws {
        let (bridge, _, view, output) = try fixture()
        defer { bridge.closeAll() }
        view.keyDown(with: event(.keyDown))
        bridge.setSuspended(true)
        view.keyDown(with: event(.keyDown, code: 0x00))
        view.keyUp(with: event(.keyUp))
        XCTAssertEqual(output.keys.map(\.pressed), [true, false])
        bridge.setSuspended(false)
        view.keyDown(with: event(.keyDown))
        view.keyUp(with: event(.keyUp))
        XCTAssertEqual(output.keys.map(\.pressed), [true, false, true, false])
    }
}
