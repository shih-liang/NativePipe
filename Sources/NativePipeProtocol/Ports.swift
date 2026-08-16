import Foundation

/// vsock / TCP port assignments for the NativePipe control and media planes.
///
/// Inside a LightHouse VM these ride vsock. For remote Linux they listen on
/// TCP (typically loopback) and are reached through `ssh -L`.
public enum NativePipePort {
    /// guestd's main RPC endpoint. The host connects, the guest listens.
    public static let control: UInt32 = 1024

    /// Window metadata: NPIP frames of `Windowing.GuestEvent` / `HostCommand`.
    public static let surface: UInt32 = 1025

    /// Encoded pixel channel (NPEN + H.264 Annex-B). Remote display only;
    /// the local VM path does not use this port.
    public static let media: UInt32 = 1026

    /// Reserved: xdg-desktop-portal backend requests.
    public static let portal: UInt32 = 1027

    /// Reserved: clipboard bulk transfer (when not piggybacked on surface).
    public static let pasteboard: UInt32 = 1028

    /// Agent file pull. The **host listens**; the guest dials
    /// `VMADDR_CID_HOST` and requests a named file (guestd, then unit files).
    public static let agent: UInt32 = 1029

    /// Interactive PTY exec. After control `exec` succeeds, the host dials
    /// the guest on this port and the guest accepts once per session.
    public static let exec: UInt32 = 1030
}

/// The guest CID for a VZ virtual machine is always 3; 2 is the host.
public enum NativePipeCID {
    public static let host: UInt32 = 2
    public static let guest: UInt32 = 3
}
