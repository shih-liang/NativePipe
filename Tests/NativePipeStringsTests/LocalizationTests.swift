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
}
