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
    public var onApplicationsChanged: (() -> Void)?
    public var onStateChange: ((RemoteSession.State) -> Void)?
    public var applications: [GuestApplication] { session.applicationClient.cached ?? [] }

    public init(command: SSHCommand, environment: [String: String]? = nil) {
        session = RemoteSession(command: command, environment: environment)
        frames = RemoteFrameSource()
        bridge = WindowBridge(frameSource: frames)
        bridge.fileAccess = RemoteUserFileAccess(command: command, environment: environment)
        bridge.applicationIconProvider = { [weak self] id in
            self?.applications.first(where: { $0.matches(applicationID: id) })
                .flatMap { $0.iconData.flatMap(NSImage.init(data:)) }
        }
        bridge.output = { [weak self] command in
            self?.session.send(command)
        }
        bridge.onApplicationWindowMapped = { [weak self] _ in
            Task { @MainActor [weak self] in _ = try? await self?.refreshApplications() }
        }
        frames.setFrameAvailableHandler { [weak self] _ in
            self?.bridge.retryPendingFrames()
        }
        session.applicationClient.onChanged = { [weak self] in
            self?.bridge.refreshApplicationIcons()
            self?.onApplicationsChanged?()
            if self?.bridge.dockWindows.isEmpty == false {
                Task { @MainActor [weak self] in _ = try? await self?.refreshApplications() }
            }
        }
        session.onEvent = { [weak self] event in
            self?.handle(event)
        }
        session.onMediaFrame = { [weak self] header, payload in
            self?.frames.ingest(header: header, payload: payload)
        }
        session.onStateChange = { [weak self] state in
            if state == .disconnected {
                self?.bridge.closeAll()
                self?.frames.removeAll()
            }
            self?.onStateChange?(state)
        }
    }

    public func connect() async throws {
        // A caller may replace a live transport without waiting for EOF. Drop
        // the previous transport generation's authoritative window state
        // before the new compositor replays its own state after channelReady.
        bridge.closeAll()
        frames.removeAll()
        try await session.connect()
    }

    public func disconnect() {
        session.disconnect()
        bridge.closeAll()
        frames.removeAll()
    }

    @discardableResult
    public func refreshApplications(refresh: Bool = false) async throws -> [GuestApplication] {
        let applications = try await session.applications(refresh: refresh)
        bridge.refreshApplicationIcons()
        return applications
    }

    private func handle(_ event: Windowing.GuestEvent) {
        if case .surfaceDestroyed(let surface) = event {
            frames.removeSurface(surface)
        }
        bridge.apply(event)
    }
}
