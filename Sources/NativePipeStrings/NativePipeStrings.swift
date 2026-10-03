import Foundation

/// Only human-readable text belongs in this catalog. Keep configuration values,
/// protocol fields, commands, application names and user-supplied text unchanged.
public func NPText(_ key: String, _ arguments: String...) -> String {
    NPStrings.text(key, arguments: arguments)
}

public enum NPStrings {
    // Native resource directories are the language inventory; SwiftPM may
    // lowercase them, so normalize the public identifiers for both builders.
    public static let supportedLanguages = NativePipeResources.bundle.localizations
        .filter { $0 != "Base" }
        .map { Locale.canonicalLanguageIdentifier(from: $0) }
        .sorted()

    /// An explicit language is useful for previews and tests. Production follows
    /// the language macOS selects for the application and this resource bundle.
    public static func text(_ key: String, arguments: [String] = [], language: String? = nil) -> String {
        let bundle = localizedBundle(language)
        let format = bundle.localizedString(forKey: key, value: key, table: "Localizable")
        guard !arguments.isEmpty else { return format }
        // Every placeholder accepts a string. Positional placeholders allow
        // translations to reorder arguments without exposing printf type/ABI mismatches.
        return String(format: format, locale: language.map(Locale.init(identifier:)) ?? .current,
                      arguments: arguments.map { $0 as NSString })
    }

    // A plain CLI has no main-bundle localizations. Let Foundation match the
    // user's language directly instead of inheriting that bundle's English
    // fallback. Cache the choice for this process, as native apps normally do.
    private static let preferredBundle: Bundle = {
        let language = Bundle.preferredLocalizations(from: supportedLanguages,
            forPreferences: Locale.preferredLanguages).first ?? "en"
        return localizedBundle(language)
    }()

    static func localizedBundle(_ language: String?) -> Bundle {
        guard let language else { return preferredBundle }
        let selected = supportedLanguages.contains(language) ? language : "en"
        // SwiftPM's native builder lowercases language directory names;
        // Xcode preserves their canonical spelling. Use the bundle's actual
        // localization name because resource lookup is case-sensitive.
        let resourceName = NativePipeResources.bundle.localizations.first {
            $0.caseInsensitiveCompare(selected) == .orderedSame
        } ?? "en"
        guard let url = NativePipeResources.bundle.url(forResource: resourceName, withExtension: "lproj"),
              let bundle = Bundle(url: url) else { return NativePipeResources.bundle }
        return bundle
    }

}
