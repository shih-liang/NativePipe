/// Directions supported by the shared SFTP filesystem transfer engine.
public enum SFTPTransfer {
    public enum Direction: String, Identifiable, Sendable {
        case upload, download
        public var id: String { rawValue }
    }
}
