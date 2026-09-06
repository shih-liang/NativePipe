import Foundation
import NativePipeProtocol
import XCTest

final class KeyboardShortcutTests: XCTestCase {
    func testDefaultsAndQuitMapping() {
        let preferences = KeyboardShortcutPreferences()
        XCTAssertNil(preferences.validationError)
        for key: ShortcutKey in [.c, .v, .s, .q] {
            XCTAssertEqual(preferences.rule(for: .init(key, .logo), applicationID: "gtk4-demo")?.target,
                           .init(key, .control))
        }
        XCTAssertNil(preferences.rule(for: .init(.c, .control), applicationID: "qterminal"))
        XCTAssertNil(preferences.rule(for: .init(.c, [.control, .logo]), applicationID: "gtk4-demo"))
    }

    func testApplicationOverridesAndExplicitPassthrough() throws {
        let preferences = KeyboardShortcutPreferences()
        XCTAssertEqual(preferences.rule(for: .init(.v, .logo), applicationID: "qterminal")?.target,
                       .init(.v, [.control, .shift]))
        let bypass = try XCTUnwrap(preferences.rule(for: .init(.s, .logo), applicationID: "foot"))
        XCTAssertNil(bypass.target, "Cmd+S must not send XOFF to a terminal")
        XCTAssertNotNil(preferences.rule(for: .init(.t, .logo), applicationID: "firefox"))
        XCTAssertNil(preferences.rule(for: .init(.t, .logo), applicationID: "gtk4-demo"))
        XCTAssertEqual(preferences.rule(for: .init(.q, .logo), applicationID: "org.gnome.Terminal")?.target,
                       .init(.q, [.control, .shift]))
    }

    func testDisabledRuleAndMasterSwitch() {
        var preferences = KeyboardShortcutPreferences()
        let index = preferences.rules.firstIndex { $0.applicationIDs?.contains("qterminal") == true && $0.source.key == .c }!
        preferences.rules[index].enabled = false
        XCTAssertEqual(preferences.rule(for: .init(.c, .logo), applicationID: "qterminal")?.target,
                       .init(.c, .control))
        preferences.enabled = false
        XCTAssertNil(preferences.rule(for: .init(.q, .logo), applicationID: "firefox"))
        XCTAssertFalse(preferences.rules.isEmpty)
    }

    func testDuplicateScopeRejectedButGlobalAndAppScopesMayOverlap() {
        let global = KeyboardShortcutRule(id: 1, source: .init(.c, .logo), target: .init(.c, .control))
        var other = global
        other.id = 2
        XCTAssertNotNil(KeyboardShortcutPreferences(rules: [global, other]).validationError)
        other.applicationIDs = ["editor"]
        XCTAssertNil(KeyboardShortcutPreferences(rules: [global, other]).validationError)
        var third = other
        third.id = 3
        third.applicationIDs = ["editor", "browser"]
        XCTAssertNotNil(KeyboardShortcutPreferences(rules: [other, third]).validationError)
        third.enabled = false
        XCTAssertNotNil(KeyboardShortcutPreferences(rules: [other, third]).validationError)
        third.applicationIDs = ["browser"]
        XCTAssertNil(KeyboardShortcutPreferences(rules: [other, third]).validationError)
    }

    func testSavingWhileRemappingIsOffStillRejectsConflicts() throws {
        let first = KeyboardShortcutRule(id: 1, source: .init(.s, .logo), target: .init(.s, .control))
        var duplicate = first
        duplicate.id = 2
        duplicate.enabled = false
        let preferences = KeyboardShortcutPreferences(enabled: false, rules: [first, duplicate])
        let error = try XCTUnwrap(preferences.validationError)
        XCTAssertTrue(error.contains("⌘S"))
        XCTAssertTrue(error.contains("all applications"))
        XCTAssertThrowsError(try preferences.validate())
    }

    func testEditingReplacesTheRuleBeforeConflictValidation() throws {
        var preferences = KeyboardShortcutPreferences(rules: [
            .init(id: 1, applicationIDs: ["editor", "terminal"],
                  source: .init(.s, .logo), target: .init(.s, .control)),
            .init(id: 2, applicationIDs: ["editor"],
                  source: .init(.o, .logo), target: .init(.o, .control)),
        ])
        preferences.rules[0].target = .init(.s, [.control, .shift])
        XCTAssertNil(preferences.validationError, "A rule must not conflict with itself")
        preferences.rules[1].source = .init(.s, .logo)
        XCTAssertTrue(try XCTUnwrap(preferences.validationError).contains("editor"))
    }

    func testBinaryPreferencesRoundTripIncludingPassthrough() throws {
        var preferences = KeyboardShortcutPreferences()
        preferences.rules.append(.init(id: 1000, applicationIDs: ["custom-app"],
                                       source: .init(.f12, [.control, .alt]), target: .init(.home)))
        let encoder = PropertyListEncoder()
        encoder.outputFormat = .binary
        let data = try encoder.encode(preferences)
        XCTAssertEqual(try PropertyListDecoder().decode(KeyboardShortcutPreferences.self, from: data), preferences)
    }
}
