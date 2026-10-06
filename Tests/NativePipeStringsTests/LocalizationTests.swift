import Foundation
import XCTest
@testable import NativePipeStrings

final class LocalizationTests: XCTestCase {
    private func dictionary(_ language: String) throws -> [String: String] {
        let url = try XCTUnwrap(NPStrings.localizedBundle(language).url(forResource: "Localizable", withExtension: "strings"))
        return try XCTUnwrap(PropertyListSerialization.propertyList(from: Data(contentsOf: url), format: nil) as? [String: String])
    }

    // Argument positions and ABI types must survive translation, including
    // positional reordering. A literal percent sign does not consume a value.
    private func placeholders(_ format: String) throws -> [String] {
        let expression = try NSRegularExpression(pattern: #"%%|%(?:(\d+)\$)?[-+0# ]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(hh|h|ll|l|z|t|j|L)?([@diuoxXfFeEgGaAcCsSp])"#)
        let string = format as NSString
        var next = 0
        return expression.matches(in: format, range: NSRange(location: 0, length: string.length)).compactMap { match in
            if string.substring(with: match.range) == "%%" { return nil }
            next += 1
            let position = match.range(at: 1).location == NSNotFound ? String(next) : string.substring(with: match.range(at: 1))
            let size = match.range(at: 2).location == NSNotFound ? "" : string.substring(with: match.range(at: 2))
            return position + ":" + size + string.substring(with: match.range(at: 3))
        }.sorted()
    }

    func testEveryLanguageHasTheSameKeysAndArgumentTypes() throws {
        let english = try dictionary("en")
        XCTAssertGreaterThan(english.count, 100)
        for language in NPStrings.supportedLanguages {
            let translated = try dictionary(language)
            XCTAssertEqual(Set(english.keys), Set(translated.keys), language)
            for (key, value) in translated {
                XCTAssertFalse(value.isEmpty, "\(language): \(key)")
                XCTAssertEqual(try placeholders(english[key]!), try placeholders(value), "\(language): \(key)")
            }
        }
    }

    func testBundledLanguagesAndRegionalPreferences() {
        XCTAssertEqual(Set(NPStrings.supportedLanguages),
            Set(["en", "zh-Hans", "zh-Hant", "ja", "ko", "fr", "de", "es", "pt-BR", "it", "ru"]))
        for (preference, expected) in [("ja-JP", "ja"), ("ko-KR", "ko"), ("fr-CA", "fr"),
            ("de-AT", "de"), ("es-MX", "es"), ("pt-BR", "pt-BR"), ("it-IT", "it"), ("ru-RU", "ru")] {
            XCTAssertEqual(Bundle.preferredLocalizations(from: NPStrings.supportedLanguages,
                forPreferences: [preference]).first, expected, preference)
            XCTAssertNotEqual(NPStrings.text("Cancel", language: expected), "Cancel", expected)
        }
    }

    func testFallbackAndUserSuppliedTextArePreserved() {
        let argument = "测试 %s /tmp/München"
        XCTAssertEqual(NPStrings.text("Keychain: %@", arguments: [argument], language: "en"), "Keychain: " + argument)
        XCTAssertEqual(NPStrings.text("Keychain: %@", arguments: [argument], language: "zh-Hans"), "钥匙串：" + argument)
        XCTAssertEqual(NPStrings.text("Keychain: %@", arguments: [argument], language: "zh-Hant"), "鑰匙圈：" + argument)
        XCTAssertEqual(NPStrings.text("Cancel", language: "zz"), "Cancel")
        XCTAssertEqual(NPStrings.text("a-user-supplied-window-title", language: "zh-Hans"), "a-user-supplied-window-title")
    }

    func testDefaultBundleFollowsItsPreferredLanguage() {
        let bundle = NPStrings.localizedBundle(nil)
        let language = Bundle.preferredLocalizations(from: NPStrings.supportedLanguages,
            forPreferences: Locale.preferredLanguages).first ?? "en"
        XCTAssertEqual(bundle.bundleURL.lastPathComponent.lowercased(), language.lowercased() + ".lproj")
        if let expected = ProcessInfo.processInfo.environment["LOCALIZATION_LANGUAGE"] {
            XCTAssertEqual(language, expected, "The native bundle must follow the launch language")
        }
        XCTAssertEqual(NPText("Cancel"), NPStrings.text("Cancel", language: language))
    }

    /// The test above keeps the translation tables in step with one another,
    /// but not with the code. Keys are the English sentences themselves, so
    /// editing a sentence in Swift without editing the tables still shows
    /// correct English -- an unknown key falls back to itself -- while every
    /// other language silently reverts to English for that message. This reads
    /// each literal NPText key out of the sources and requires it in the table.
    func testEverySourceKeyIsInTheEnglishTable() throws {
        let english = Set(try dictionary("en").keys)
        let sources = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("Sources")
        let call = try NSRegularExpression(pattern: #"NPText\(\s*"((?:[^"\\]|\\.)*)""#)
        let files = try XCTUnwrap(FileManager.default.enumerator(at: sources, includingPropertiesForKeys: nil))
        var checked = 0
        for case let file as URL in files where file.pathExtension == "swift" {
            let text = try String(contentsOf: file, encoding: .utf8)
            let range = NSRange(text.startIndex..., in: text)
            for match in call.matches(in: text, range: range) {
                let literal = String(text[Range(match.range(at: 1), in: text)!])
                let key = try XCTUnwrap(Self.unescape(literal),
                    "\(file.lastPathComponent): interpolation in an NPText key cannot be translated: \(literal)")
                XCTAssertTrue(english.contains(key), "\(file.lastPathComponent): missing from en.lproj: \(key)")
                checked += 1
            }
        }
        // Guards the scan itself: a moved Sources directory or a broken pattern
        // would otherwise make this pass by checking nothing.
        XCTAssertGreaterThan(checked, 100)
    }

    /// Swift string-literal escapes, enough for localization keys. Returns nil
    /// for interpolation, which makes a key dynamic and so untranslatable.
    private static func unescape(_ literal: String) -> String? {
        var result = "", characters = Array(literal), index = 0
        while index < characters.count {
            let character = characters[index]
            guard character == "\\", index + 1 < characters.count else {
                result.append(character); index += 1; continue
            }
            let next = characters[index + 1]
            switch next {
            case "n": result.append("\n")
            case "t": result.append("\t")
            case "r": result.append("\r")
            case "0": result.append("\0")
            case "\"", "'", "\\": result.append(next)
            case "(": return nil
            case "u":
                guard let close = characters[index...].firstIndex(of: "}"),
                      let scalar = UInt32(String(characters[(index + 3)..<close]), radix: 16)
                        .flatMap(Unicode.Scalar.init) else { return nil }
                result.unicodeScalars.append(scalar)
                index = close + 1
                continue
            default: return nil
            }
            index += 2
        }
        return result
    }
}
