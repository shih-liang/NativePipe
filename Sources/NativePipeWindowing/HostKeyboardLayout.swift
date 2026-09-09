import AppKit
import Carbon

public enum HostKeyboardLayout {
    public static func current() -> String {
        guard let source = TISCopyCurrentKeyboardInputSource()?.takeRetainedValue(),
              let pointer = TISGetInputSourceProperty(source, kTISPropertyInputSourceID)
        else { return "us" }
        let identifier = Unmanaged<CFString>.fromOpaque(pointer).takeUnretainedValue() as String
        let key = identifier.split(separator: ".").last.map(String.init) ?? identifier
        let layouts = [
            "US": "us", "ABC": "us", "British": "gb", "Canadian": "ca",
            "German": "de", "SwissGerman": "ch", "French": "fr",
            "Spanish": "es", "Italian": "it", "Portuguese": "pt",
            "Dutch": "nl", "Swedish": "se", "Norwegian": "no",
            "Danish": "dk", "Finnish": "fi", "Polish": "pl",
            "Czech": "cz", "Russian": "ru", "Ukrainian": "ua",
            "Japanese": "jp", "Korean": "kr",
        ]
        return layouts[key.replacingOccurrences(of: "-", with: "")] ?? "us"
    }
}
