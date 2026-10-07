/// Revocation is scoped to the user action that authorized a mounted item.
/// Turning off clipboard sharing must not invalidate an approved Open request.
public enum GuestFileSharePurpose: String, Codable, Hashable, Sendable {
    case clipboard, drag, open, files
}
