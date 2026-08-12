import Foundation

/// vsock port assignments for the NativePipe control plane.
///
/// The control plane deliberately does not ride on TCP/IP: DHCP failures, VPN
/// changes or a guest bringing its own networking must never be able to take
/// the window system down. Everything host <-> guest travels over vsock.
public enum NativePipePort {
    /// guestd's main RPC endpoint. The host connects, the guest listens.
    public static let control: UInt32 = 1024

    /// Reserved: bulk surface/damage traffic for the Wayland bridge.
    public static let surface: UInt32 = 1025

    /// Reserved: clipboard and drag-and-drop payload transfer.
    public static let pasteboard: UInt32 = 1026

    /// Reserved: xdg-desktop-portal backend requests.
    public static let portal: UInt32 = 1027
}

/// The guest CID for a VZ virtual machine is always 3; 2 is the host.
public enum NativePipeCID {
    public static let host: UInt32 = 2
    public static let guest: UInt32 = 3
}
