import AppKit
import Foundation
import NativePipeProtocol
import NativePipeWindowing
import NativePipeStrings

/// Wires `RemoteSession` + `RemoteFrameSource` into `WindowBridge`.
@MainActor
public final class RemoteDisplayController {
    public let session: RemoteSession
    public let frames: RemoteFrameSource
    public let bridge: WindowBridge
    private struct SceneID: Hashable { let surface: UInt32; let presentation: UInt32 }
    private var sceneSources: [SceneID: [Windowing.SceneLayer]] = [:]
    private var hostOpen: GuestOpenCoordinator?
    private var hostOpenRequests: [UInt32: Task<Void, Never>] = [:]
    private var hostOpenGeneration = 0
    private var guestNotifications: GuestNotificationPresenter?
    private let notificationIdentity: String
    /// LinPortal supplies its preference; standalone NativePipe still asks for
    /// user approval for each item through the shared macOS coordinator.
    public var hostOpenEnabled: () -> Bool = { true }
    public var notificationsEnabled: () -> Bool = { true }
    public var notificationResponseDirectory: URL = FileManager.default.temporaryDirectory
    public var onApplicationsChanged: (() -> Void)?
    public var onStateChange: ((RemoteSession.State) -> Void)?
    public var applications: [GuestApplication] { session.applicationClient.cached ?? [] }

    public init(command: SSHCommand, environment: [String: String]? = nil,
                localCompositorDirectory: URL? = nil) {
        notificationIdentity = "ssh:" + command.credentialID
        session = RemoteSession(command: command, environment: environment,
                                localCompositorDirectory: localCompositorDirectory)
        frames = RemoteFrameSource()
        bridge = WindowBridge(frameSource: frames)
        bridge.fileAccess = RemoteUserFileAccess(command: command, environment: environment)
        bridge.applicationIconProvider = { [weak self] id in
            self?.applications.first(where: { $0.matches(applicationID: id) })
                .flatMap { $0.iconData.flatMap(NSImage.init(data:)) }
        }
        bridge.applicationNameProvider = { [weak self] id in
            self?.applications.first(where: { $0.matches(applicationID: id) })?.name
        }
        bridge.output = { [weak self] command in
            self?.session.send(command)
        }
        bridge.onGuestNotification = { [weak self] in self?.guestNotifications?.post($0) }
        bridge.onGuestNotificationClosed = { [weak self] id, revision in
            self?.guestNotifications?.close(id: id, revision: revision)
        }
        bridge.onGuestNotificationBacklogReset = { [weak self] in self?.guestNotifications?.resetBacklog() }
        bridge.onScenePresentation = { [weak self] surface, id, displayed, interval in
            guard let self else { return }
            let layers = self.sceneSources.removeValue(forKey: SceneID(surface: surface, presentation: id))
            if !displayed, let layers {
                self.frames.whenConsumed(layers) { [weak self] in
                    self?.session.sceneCompleted(surface: surface, presentationID: id, displayed: false, intervalNanoseconds: interval)
                }
            } else {
                self.session.sceneCompleted(surface: surface, presentationID: id, displayed: displayed, intervalNanoseconds: interval)
            }
        }
        bridge.onApplicationWindowMapped = { [weak self] _ in
            Task { @MainActor [weak self] in _ = try? await self?.refreshApplications() }
        }
        frames.setFrameAvailableHandler { [weak self] _ in
            self?.bridge.retryPendingFrames()
        }
        frames.onFailure = { [weak self] message in self?.session.decodingFailed(message) }
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
        session.onMediaFrame = { [frames] header, payload in
            frames.ingest(header: header, payload: payload)
        }
        session.onStateChange = { [weak self] state in
            if state == .disconnected {
                self?.stopNotifications()
                self?.stopHostOpen()
                self?.bridge.closeAll()
                self?.sceneSources.removeAll()
                self?.frames.removeAll()
            }
            self?.onStateChange?(state)
        }
    }

    public func connect() async throws {
        // A caller may replace a live transport without waiting for EOF. Drop
        // the previous transport generation's authoritative window state
        // before the new compositor replays its own state after channelReady.
        stopHostOpen()
        stopNotifications()
        session.disconnect()
        bridge.closeAll()
        sceneSources.removeAll()
        frames.removeAll()
        try await session.connect()
    }

    public func disconnect() {
        stopHostOpen()
        stopNotifications()
        session.disconnect()
        bridge.closeAll()
        sceneSources.removeAll()
        frames.removeAll()
    }

    @discardableResult
    public func refreshApplications(refresh: Bool = false) async throws -> [GuestApplication] {
        let applications = try await session.applications(refresh: refresh)
        bridge.refreshApplicationIcons()
        return applications
    }

    private func handle(_ event: Windowing.GuestEvent) {
        switch event {
        case .channelReady:
            if let guestNotifications {
                // A replacement host handshake has a new guest ID namespace.
                // Withdraw old macOS banners without echoing their IDs to it.
                guestNotifications.resetSession()
            } else {
                guestNotifications = GuestNotificationPresenter(machine: bridge.machineName,
                    identity: notificationIdentity, responseDirectory: notificationResponseDirectory,
                    isEnabled: { [weak self] in self?.notificationsEnabled() ?? false },
                    send: { [weak self] in self?.session.send($0) })
            }
        case .hostOpenRequested(let token, let request):
            receiveHostOpen(token: token, request: request)
            return
        case .hostOpenCancelled(let token):
            hostOpenRequests.removeValue(forKey: token)?.cancel()
            return
        case .hostOpenRejected(let token):
            if let token {
                hostOpenRequests.removeValue(forKey: token)?.cancel()
                session.send(.hostOpenResponse(token: token, response: .init(status: .failed,
                    message: NPText("That request was not understood."))))
            }
            return
        default: break
        }
        if case .sceneCommitted(let scene) = event {
            sceneSources[SceneID(surface: scene.surface, presentation: scene.presentationID)] = scene.layers
        }
        if case .surfaceDestroyed(let surface) = event {
            frames.removeSurface(surface)
        }
        bridge.apply(event)
    }

    private func receiveHostOpen(token: UInt32, request: HostOpenWire.Request) {
        guard hostOpenRequests[token] == nil else { return }
        guard hostOpenRequests.count < 16, let access = bridge.fileAccess else {
            session.send(.hostOpenResponse(token: token, response: .init(status: .refused,
                message: NPText("Another request is waiting. Try again in a moment."))))
            return
        }
        if hostOpen == nil {
            hostOpen = GuestOpenCoordinator(machineName: bridge.machineName,
                isEnabled: { [weak self] in self?.hostOpenEnabled() ?? false }, fileAccess: access,
                publishGuestFiles: bridge.publishGuestFiles)
        }
        guard let coordinator = hostOpen else { return }
        let generation = hostOpenGeneration
        hostOpenRequests[token] = Task { @MainActor [weak self] in
            let response = await coordinator.handle(request)
            guard !Task.isCancelled, let self, self.hostOpenGeneration == generation,
                  self.hostOpenRequests.removeValue(forKey: token) != nil else { return }
            self.session.send(.hostOpenResponse(token: token, response: response))
        }
    }

    private func stopHostOpen() {
        hostOpenGeneration &+= 1
        hostOpenRequests.values.forEach { $0.cancel() }
        hostOpenRequests.removeAll()
        hostOpen?.stop()
        hostOpen = nil
    }

    public func refreshNotificationPreferences() { guestNotifications?.refreshPreferences() }
    private func stopNotifications() { guestNotifications?.stop(); guestNotifications = nil }

    deinit { hostOpenRequests.values.forEach { $0.cancel() } }
}
