import Foundation

/// Port assignments shared by the local VM and remote transports.
public enum NativePipePort {
    public static let control: UInt32 = 1024
    public static let surface: UInt32 = 1025
    public static let media: UInt32 = 1026
    public static let portal: UInt32 = 1027
    public static let pasteboard: UInt32 = 1028
    public static let agent: UInt32 = 1029

    public static let sessionFirst: UInt32 = 2048
    public static let sessionLast: UInt32 = 2303

    public static let windowEvent: UInt32 = 4096
    public static let windowControl: UInt32 = 4097
    public static let windowInput: UInt32 = 4098
    public static let windowFeedback: UInt32 = 4099
}

public enum NativePipeCID {
    public static let host: UInt32 = 2
    public static let guest: UInt32 = 3
}
