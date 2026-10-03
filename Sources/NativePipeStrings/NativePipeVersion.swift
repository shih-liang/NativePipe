import Foundation

/// The same release version is carried by the CLI resource bundle and all guest archives.
public enum NativePipeVersion {
    public static let current: String = {
        guard let url = NativePipeResources.bundle.url(forResource: "VERSION", withExtension: nil),
              let value = try? String(contentsOf: url, encoding: .utf8) else {
            preconditionFailure("NativePipe release version resource is missing")
        }
        return value.trimmingCharacters(in: .whitespacesAndNewlines)
    }()
}
