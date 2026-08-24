import Foundation

/// Port assignments for NativePipe's independent services.
public enum NativePipePort {
    // guestd: low, fixed control-plane ports.
    /// guestd's main RPC endpoint. The host connects; the guest listens.
    public static let control: UInt32 = 1024

    /// Agent file pull. The host listens; guestd/bootstrap connects.
    public static let agent: UInt32 = 1029

    // RemotePipe: legacy loopback TCP ports reached through ssh -L. They are
    // deliberately not reused by the local VM's vsock compositor transport.
    public static let surface: UInt32 = 1025
    public static let media: UInt32 = 1026

    // Interactive sessions: guestd allocates one temporary PTY listener from
    // this range for every exec session.
    public static let sessionFirst: UInt32 = 2048
    public static let sessionLast: UInt32 = 2303

    // Local VM compositor: independent vsock streams. A blocked frame-release
    // write must never delay a resize or input event.
    /// Guest-to-host window events.
    public static let windowEvent: UInt32 = 4096
    /// Host-to-guest window state: configure, scale, close and clipboard.
    public static let windowControl: UInt32 = 4097
    /// Host-to-guest pointer, keyboard and text-input events.
    public static let windowInput: UInt32 = 4098
    /// Host-to-guest frame callback, FIFO and buffer-release feedback.
    public static let windowFeedback: UInt32 = 4099

    /// Reserved: xdg-desktop-portal backend requests.
    public static let portal: UInt32 = 1027

    /// Reserved: clipboard bulk transfer (when not piggybacked on surface).
    public static let pasteboard: UInt32 = 1028

}

/// The guest CID for a VZ virtual machine is always 3; 2 is the host.
public enum NativePipeCID {
    public static let host: UInt32 = 2
    public static let guest: UInt32 = 3
}
