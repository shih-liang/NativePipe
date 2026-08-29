/// Loopback TCP endpoints on the Linux host. The macOS client reaches them
/// through SSH local forwards by default.
public enum NativePipePort {
    public static let surface: UInt32 = 1025
    public static let media: UInt32 = 1026
}
