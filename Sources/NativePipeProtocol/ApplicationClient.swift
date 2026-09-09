import Foundation

/// One catalog/cache and request implementation shared by VMHost and Remote.
@MainActor
public final class ApplicationClient {
    public nonisolated static let maximumMetadataBytes = 16 * 1024 * 1024
    public nonisolated static let maximumIconBytes = 16 * 1024 * 1024
    public var onChanged: (() -> Void)?
    public var onProcessExited: ((Int32, Int32) -> Void)?
    public private(set) var revision = UInt64.random(in: 1...UInt64.max / 2)
    public private(set) var cached: [GuestApplication]?
    private let send: (Windowing.HostCommand) -> Void
    private var connected = false
    private var nextToken: UInt32 = 0
    private var catalogTask: Task<[GuestApplication], Error>?
    private var iconTask: Task<Void, Never>?
    private var taskGeneration: UInt64 = 0
    private struct Request {
        let action: ApplicationAction
        let continuation: CheckedContinuation<ApplicationReply, Error>
        var apps: [GuestApplication] = []
        var ids: Set<String> = []
        var bytes = 0
    }
    private var pending: [UInt32: Request] = [:]

    public struct Failure: LocalizedError {
        public let message: String
        public var errorDescription: String? { message }
        public init(_ message: String) { self.message = message }
    }
    public init(send: @escaping (Windowing.HostCommand) -> Void) { self.send = send }

    public func setConnected(_ ready: Bool) {
        guard connected != ready else { return }
        connected = ready
        revision &+= 1
        taskGeneration &+= 1
        cached = nil
        catalogTask?.cancel(); catalogTask = nil
        iconTask?.cancel(); iconTask = nil
        let requests = pending; pending.removeAll()
        for request in requests.values { request.continuation.resume(throwing: CancellationError()) }
        onChanged?()
    }
    public func receive(_ reply: ApplicationReply) {
        switch reply {
        case .changed:
            iconTask?.cancel(); iconTask = nil
            revision &+= 1; cached = nil; onChanged?()
        case .exited(let pid, let status): onProcessExited?(pid, status)
        case .batch(let token, let apps):
            guard var request = pending[token], request.action == .list else { return }
            for app in apps {
                guard request.ids.insert(app.id).inserted else {
                    finish(token, .failure(Failure("Duplicate application ID in catalog."))); return
                }
                request.bytes += app.metadataByteCount
            }
            // A malicious peer must fail explicitly rather than exhaust host memory.
            guard request.bytes <= Self.maximumMetadataBytes, request.ids.count <= 65_536 else {
                finish(token, .failure(Failure("Application catalog exceeds the safety limit."))); return
            }
            request.apps += apps; pending[token] = request
        case .end(let token, let message):
            guard let request = pending[token], request.action == .list || !message.isEmpty else { return }
            finish(token, message.isEmpty ? .success(.batch(token, request.apps)) : .failure(Failure(message)))
        case .launched(let token, _, let message):
            guard pending[token]?.action == .launch || pending[token]?.action == .appearance else { return }
            finish(token, message.isEmpty ? .success(reply) : .failure(Failure(message)))
        case .icon(let token, _):
            guard pending[token]?.action == .icon else { return }
            finish(token, .success(reply))
        }
    }
    private func finish(_ token: UInt32, _ result: Result<ApplicationReply, Error>) {
        pending.removeValue(forKey: token)?.continuation.resume(with: result)
    }
    private func request(_ action: ApplicationAction, _ id: String = "") async throws -> ApplicationReply {
        guard connected else { throw Failure("The Linux window service is disconnected.") }
        repeat { nextToken &+= 1 } while nextToken == 0 || pending[nextToken] != nil
        let token = nextToken
        let timeout = Task { [weak self] in
            do { try await Task.sleep(for: .seconds(25)) } catch { return }
            self?.finish(token, .failure(Failure("The Linux application request timed out.")))
        }
        defer { timeout.cancel() }
        return try await withTaskCancellationHandler {
            try Task.checkCancellation()
            return try await withCheckedThrowingContinuation { continuation in
                pending[token] = Request(action: action, continuation: continuation)
                send(.applicationRequest(token: token, action: action, application: id))
            }
        } onCancel: { Task { @MainActor [weak self] in self?.finish(token, .failure(CancellationError())) } }
    }
    public func applications(refresh: Bool = false) async throws -> [GuestApplication] {
        if let task = catalogTask { return try await task.value }
        if !refresh, let cached { return cached }
        iconTask?.cancel(); iconTask = nil
        taskGeneration &+= 1
        let generation = taskGeneration
        let task = Task { [self] in
            while true {
                let version = revision
                guard case .batch(_, var apps) = try await request(.list) else { throw Failure("Invalid catalog reply.") }
                try Task.checkCancellation()
                if version != revision { continue }
                apps.sort { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
                cached = apps
                // Publish names immediately. Slow icons must not hold up status,
                // window switching, or applications on another computer.
                iconTask = Task { [weak self] in
                    await self?.loadIcons(apps, generation: generation)
                }
                return apps
            }
        }
        catalogTask = task
        defer { if generation == taskGeneration { catalogTask = nil } }
        return try await task.value
    }

    private func loadIcons(_ apps: [GuestApplication], generation: UInt64) async {
        do {
            try await withThrowingTaskGroup(of: (Int, Data).self) { group in
                    var next = 0
                    var iconBytes = 0
                    func add(_ i: Int) {
                        let id = apps[i].id
                        group.addTask { [self] in
                            guard case .icon(_, let data) = try await request(.icon, id) else {
                                throw Failure("Invalid icon reply.")
                            }
                            return (i, data)
                        }
                    }
                    while next < min(4, apps.count) { add(next); next += 1 }
                    while let (i, data) = try await group.next() {
                        iconBytes += data.count
                        guard iconBytes <= Self.maximumIconBytes else {
                            throw Failure("Application icons exceed the catalog safety limit.")
                        }
                        try Task.checkCancellation()
                        guard generation == taskGeneration, cached != nil else { throw CancellationError() }
                        cached?[i].iconData = data.isEmpty ? nil : data
                        revision &+= 1
                        onChanged?()
                        if next < apps.count { add(next); next += 1 }
                    }
            }
        } catch { /* Metadata remains usable when an optional icon fails. */ }
    }
    /// Zero means D-Bus activation succeeded without creating a child process.
    public func setAppearance(_ scheme: DesktopPreferences.ColorScheme) async throws {
        _ = try await request(.appearance, scheme == .dark ? "dark" : "light")
    }

    /// Zero means D-Bus activation succeeded without creating a child process.
    public func launch(_ id: String) async throws -> Int32 {
        guard !id.isEmpty, !id.contains("/"), id.utf8.count <= 4096,
              case .launched(_, let pid, _) = try await request(.launch, id) else {
            throw Failure("Invalid application ID.")
        }
        return pid
    }
}
