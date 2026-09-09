import Foundation

public enum WindowApplicationIdentity {
    public static func matches(_ applicationID: String, candidates: [String?]) -> Bool {
        func normalize(_ value: String) -> String {
            value.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
                .replacingOccurrences(of: ".desktop", with: "")
        }
        let requested = normalize(applicationID)
        return candidates.compactMap { $0 }.map(normalize).contains {
            requested == $0 || requested.hasPrefix($0 + ".")
        }
    }
}
