import AppKit
import Foundation
import NativePipeProtocol
import NativePipeWindowing

/// Connects TCP window/media ports and drives `WindowBridge` on the main actor.
///
/// Transport-agnostic display stack shared by any host that already has
/// NPIP+NPEN reachable on localhost (SSH forwards, LAN, etc.). The `remotepipe`
/// CLI only orchestrates SSH; it calls this for everything else.
@MainActor
public final class DisplaySession {
    public let controller: RemoteDisplayController

    public init(
        host: String = "127.0.0.1",
        surfacePort: UInt16 = UInt16(NativePipePort.surface),
        mediaPort: UInt16 = UInt16(NativePipePort.media)
    ) {
        controller = RemoteDisplayController(
            host: host,
            surfacePort: surfacePort,
            mediaPort: mediaPort)
    }

    public func connect() throws {
        try controller.connect()
    }

    public func disconnect() {
        controller.disconnect()
    }
}
