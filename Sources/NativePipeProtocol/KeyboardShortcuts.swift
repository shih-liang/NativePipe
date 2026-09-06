import Foundation

/// Symbolic keys keep letter shortcuts independent of the Mac's key positions.
/// NativePipe resolves the target through the active keyboard layout before
/// using the existing evdev input transport.
public enum ShortcutKey: String, Codable, CaseIterable, Sendable {
    case a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z
    case zero = "0", one = "1", two = "2", three = "3", four = "4"
    case five = "5", six = "6", seven = "7", eight = "8", nine = "9"
    case minus = "-", equal = "=", leftBracket = "[", rightBracket = "]"
    case backslash = "\\", semicolon = ";", quote = "'", grave = "`"
    case comma = ",", period = ".", slash = "/"
    case enter, tab, space, backspace, escape, delete, home, end, pageUp, pageDown
    case left, right, up, down
    case f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12

    public var title: String {
        switch self {
        case .pageUp: "Page Up"
        case .pageDown: "Page Down"
        case .left: "←"
        case .right: "→"
        case .up: "↑"
        case .down: "↓"
        default: rawValue.count == 1 ? rawValue.uppercased() : rawValue.capitalized
        }
    }
}

public struct ShortcutChord: Codable, Sendable, Equatable {
    public var key: ShortcutKey
    public var modifiers: Windowing.Modifiers

    public init(_ key: ShortcutKey, _ modifiers: Windowing.Modifiers = []) {
        self.key = key
        self.modifiers = modifiers
    }

    public func label(mac: Bool) -> String {
        var parts: [String] = []
        if modifiers.contains(.control) { parts.append(mac ? "⌃" : "Ctrl+") }
        if modifiers.contains(.alt) { parts.append(mac ? "⌥" : "Alt+") }
        if modifiers.contains(.shift) { parts.append(mac ? "⇧" : "Shift+") }
        if modifiers.contains(.logo) { parts.append(mac ? "⌘" : "Super+") }
        return parts.joined() + key.title
    }
}

public struct KeyboardShortcutRule: Codable, Sendable, Equatable, Identifiable {
    public var id: Int
    public var enabled: Bool
    /// Nil means all applications. An empty list deliberately matches none.
    /// Application IDs come from xdg_toplevel (also supplied by Xwayland).
    public var applicationIDs: [String]?
    public var source: ShortcutChord
    /// Nil explicitly bypasses a global mapping for this application.
    public var target: ShortcutChord?

    public init(id: Int, enabled: Bool = true, applicationIDs: [String]? = nil,
                source: ShortcutChord, target: ShortcutChord?) {
        self.id = id
        self.enabled = enabled
        self.applicationIDs = applicationIDs
        self.source = source
        self.target = target
    }
}

public struct KeyboardShortcutPreferences: Codable, Sendable, Equatable {
    public var enabled: Bool
    public var rules: [KeyboardShortcutRule]

    public init(enabled: Bool = true, rules: [KeyboardShortcutRule] = Self.defaults) {
        self.enabled = enabled
        self.rules = rules
    }

    public func rule(for source: ShortcutChord, applicationID: String?) -> KeyboardShortcutRule? {
        guard enabled else { return nil }
        let matching = rules.filter { $0.enabled && $0.source == source }
        if let applicationID,
           let specific = matching.first(where: { $0.applicationIDs?.contains(applicationID) == true }) {
            return specific
        }
        return matching.first { $0.applicationIDs == nil }
    }

    /// Validate saved rules even while remapping or individual rules are off;
    /// enabling them later must not introduce an ambiguous mapping.
    public var validationError: String? {
        guard Set(rules.map(\.id)).count == rules.count else { return "Shortcut rule IDs must be unique." }
        for (index, rule) in rules.enumerated() {
            let mask: UInt32 = 0x0f
            if rule.source.modifiers.rawValue & ~mask != 0 ||
                (rule.target?.modifiers.rawValue ?? 0) & ~mask != 0 {
                return "Use only Command, Control, Option and Shift in shortcuts."
            }
            if let ids = rule.applicationIDs, ids.isEmpty || ids.contains(where: { $0.isEmpty }) {
                return "Enter an application ID or choose all applications."
            }
            for previous in rules[..<index] where previous.source == rule.source {
                let scope: String?
                switch (previous.applicationIDs, rule.applicationIDs) {
                case (nil, nil): scope = "all applications"
                case (.some(let a), .some(let b)):
                    let shared = Set(a).intersection(b).sorted()
                    scope = shared.isEmpty ? nil : shared.joined(separator: ", ")
                default: scope = nil // An application override is intentional.
                }
                if let scope {
                    return "\(rule.source.label(mac: true)) already has a rule for \(scope). Edit that rule or choose another shortcut or application."
                }
            }
        }
        return nil
    }

    public func validate() throws {
        if let message = validationError {
            throw NSError(domain: "NativePipe.Shortcuts", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: message])
        }
    }

    public static var defaults: [KeyboardShortcutRule] {
        var rules: [KeyboardShortcutRule] = []
        func add(_ key: ShortcutKey, shift: Bool = false,
                 target: ShortcutChord?, apps: [String]? = nil) {
            rules.append(.init(id: rules.count + 1, applicationIDs: apps,
                               source: .init(key, shift ? [.logo, .shift] : .logo), target: target))
        }
        let common: [ShortcutKey] = [.c, .x, .v, .a, .z, .s, .o, .n, .p, .f, .w, .q]
        for key in common { add(key, target: .init(key, .control)) }
        for key: ShortcutKey in [.z, .s] { add(key, shift: true, target: .init(key, [.control, .shift])) }

        // These terminal IDs use Ctrl+Shift for clipboard shortcuts. Do not
        // turn Save/Undo/etc. into shell flow-control or job-control commands.
        let terminals = ["qterminal", "org.qterminal.QTerminal", "org.gnome.Terminal",
                         "org.gnome.Console", "org.kde.konsole", "foot", "footclient",
                         "Alacritty", "kitty", "xfce4-terminal"]
        for key in common where key != .q {
            add(key, target: [.c, .v].contains(key) ? .init(key, [.control, .shift]) : nil,
                apps: terminals)
        }
        for key: ShortcutKey in [.z, .s] { add(key, shift: true, target: nil, apps: terminals) }
        let tabbedTerminals = ["qterminal", "org.qterminal.QTerminal", "org.gnome.Terminal"]
        // Other terminals retain the editable global Cmd+Q -> Ctrl+Q rule.
        add(.q, target: .init(.q, [.control, .shift]), apps: ["org.gnome.Terminal"])
        add(.t, target: .init(.t, [.control, .shift]), apps: tabbedTerminals)

        let browsers = ["firefox", "org.mozilla.firefox", "chromium", "chromium-browser", "google-chrome"]
        for key: ShortcutKey in [.t, .l, .r, .g, .minus, .equal, .zero] {
            add(key, target: .init(key, .control), apps: browsers)
        }
        for key: ShortcutKey in [.t, .r, .g] {
            add(key, shift: true, target: .init(key, [.control, .shift]), apps: browsers)
        }
        add(.equal, shift: true, target: .init(.equal, [.control, .shift]), apps: browsers)
        add(.leftBracket, target: .init(.left, .alt), apps: browsers)
        add(.rightBracket, target: .init(.right, .alt), apps: browsers)

        let editors = ["org.gnome.TextEditor", "org.gnome.gedit", "gedit", "org.kde.kate",
                       "kate", "org.kde.kwrite", "code", "code-oss", "org.gnome.Builder"]
        let navigation: [(ShortcutChord, ShortcutChord)] = [
            (.init(.left, .logo), .init(.home)), (.init(.right, .logo), .init(.end)),
            (.init(.up, .logo), .init(.home, .control)), (.init(.down, .logo), .init(.end, .control)),
            (.init(.left, .alt), .init(.left, .control)), (.init(.right, .alt), .init(.right, .control)),
        ]
        for (source, target) in navigation {
            for shift in [false, true] {
                var source = source, target = target
                if shift { source.modifiers.insert(.shift); target.modifiers.insert(.shift) }
                rules.append(.init(id: rules.count + 1, applicationIDs: editors, source: source, target: target))
            }
        }
        return rules
    }
}
