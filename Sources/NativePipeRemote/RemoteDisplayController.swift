import AppKit
import Foundation
import NativePipeProtocol
import NativePipeWindowing

/// Wires `RemoteSession` + `RemoteFrameSource` into `WindowBridge`.
@MainActor
public final class RemoteDisplayController {
    public let session: RemoteSession
    public let frames: RemoteFrameSource
    public let bridge: WindowBridge

    public init(
        host: String = "127.0.0.1",
        surfacePort: UInt16 = UInt16(NativePipePort.surface),
        mediaPort: UInt16 = UInt16(NativePipePort.media)
    ) {
        session = RemoteSession(host: host, surfacePort: surfacePort, mediaPort: mediaPort)
        frames = RemoteFrameSource()
        bridge = WindowBridge(frameSource: frames)
        bridge.output = { [weak self] command in
            self?.session.send(command)
        }
        frames.setFrameAvailableHandler { [weak self] _ in
            self?.bridge.retryPendingGPUFrames()
        }
        session.onEvent = { [weak self] event in
            Task { @MainActor in
                self?.handle(event)
            }
        }
        session.onMediaFrame = { [weak self] header, payload in
            self?.frames.ingest(header: header, payload: payload)
        }
        session.onStateChange = { [weak self] state in
            Task { @MainActor in
                if state == .disconnected {
                    self?.bridge.closeAll()
                    self?.frames.removeAll()
                }
            }
        }
    }

    public func connect() throws {
        try session.connect()
    }

    public func disconnect() {
        session.disconnect()
        bridge.closeAll()
        frames.removeAll()
    }

    private func handle(_ event: Windowing.GuestEvent) {
        if case .surfaceDestroyed(let surface) = event {
            frames.removeSurface(surface)
        }
        if case .committed(let surface, let frame) = event {
            fputs(
                "nativepipe-remote: committed surface=\(surface) res=\(frame.resourceID) "
                    + "\(frame.width)x\(frame.height) source=\(frame.source)\n",
                stderr)
        }
        if case .toplevelCreated(let window, let surface) = event {
            fputs("nativepipe-remote: toplevel window=\(window) surface=\(surface)\n", stderr)
        }
        bridge.apply(event)
    }
}
