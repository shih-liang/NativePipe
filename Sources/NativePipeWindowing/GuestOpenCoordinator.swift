import AppKit
import CoreServices
import Darwin
import Foundation
import NativePipeProtocol
import NativePipeStrings

public enum GuestOpenPrompt: Equatable {
    case link(machine: String, url: URL)
    case receive(machine: String, path: String)
    case execute(machine: String, file: URL)
}

public enum GuestOpenAction: Equatable {
    case open
    case save(URL)
    case cancel
}

/// The same approval and transfer workflow serves VM vsock and SSH stdio.
/// All local items are stable private copies, including items in a VM share.
@MainActor
public final class GuestOpenCoordinator {
    private let machineName: String
    private let isEnabled: () -> Bool
    private let shares: () -> [GuestOpenPolicy.SharedFolder]
    private let fileAccess: UserFileAccess
    private let confirm: @MainActor (GuestOpenPrompt) async -> GuestOpenAction
    private let open: @MainActor (URL) async -> Bool
    private let quarantine: @MainActor (URL) async throws -> Void
    private let scratch: URL
    private let transferProgress: (@Sendable (FileTransferProgress) -> Void)?
    private var active: Task<HostOpenWire.Response, Never>?
    private var activeID: UUID?
    private var stopped = false
    private var nextRequest: TimeInterval = 0

    public init(machineName: String, isEnabled: @escaping () -> Bool = { true },
                shares: @escaping () -> [GuestOpenPolicy.SharedFolder] = { [] },
                fileAccess: UserFileAccess,
                scratch: URL = FileManager.default.temporaryDirectory.appendingPathComponent("nativepipe-received", isDirectory: true),
                confirm: @escaping @MainActor (GuestOpenPrompt) async -> GuestOpenAction = GuestOpenDialogs.confirm,
                open: @escaping @MainActor (URL) async -> Bool = GuestOpenCoordinator.openWithWorkspace,
                quarantine: @escaping @MainActor (URL) async throws -> Void = GuestOpenCoordinator.markAsDownloaded,
                transferProgress: (@Sendable (FileTransferProgress) -> Void)? = nil) {
        self.machineName = machineName; self.isEnabled = isEnabled; self.shares = shares
        self.fileAccess = fileAccess; self.scratch = scratch; self.confirm = confirm
        self.open = open; self.quarantine = quarantine
        self.transferProgress = transferProgress
    }

    public func handle(_ request: HostOpenWire.Request) async -> HostOpenWire.Response {
        guard !stopped, isEnabled() else { return disabled() }
        // One visible approval/transfer per connection owner. Concurrent guest
        // callers cannot flood the desktop with dialogs or allocate copies.
        let now = ProcessInfo.processInfo.systemUptime
        guard active == nil, now >= nextRequest else {
            return .init(status: .refused, message: NPText("Another request is waiting. Try again in a moment."))
        }
        nextRequest = now + 1
        activeID = UUID()
        let task = Task { await perform(request) }
        active = task
        let result = await withTaskCancellationHandler(operation: { await task.value }, onCancel: { task.cancel() })
        active = nil
        activeID = nil
        return result
    }

    public func stop() { stopped = true; active?.cancel() }

    private func disabled() -> HostOpenWire.Response {
        .init(status: .disabled, message: NPText("Opening on the Mac is switched off."))
    }
    private func cancelled() -> HostOpenWire.Response {
        .init(status: .refused, message: NPText("The request was cancelled."))
    }
    private func check(_ share: GuestOpenPolicy.SharedFolder? = nil) throws {
        try Task.checkCancellation()
        guard !stopped, isEnabled() else { throw CancellationError() }
        if let share, !shares().contains(share) { throw CancellationError() }
    }

    private func perform(_ request: HostOpenWire.Request) async -> HostOpenWire.Response {
        do {
            try check()
            switch GuestOpenPolicy.decide(request, shares: shares()) {
            case .refuse(let message): return .init(status: .refused, message: message)
            case .openURL(let url):
                guard await confirm(.link(machine: machineName, url: url)) == .open else { return cancelled() }
                try check()
                return await opened(url)
            case .receiveFile(let path, let name, let share):
                return await receiveFile(path: path, name: name, share: share)
            }
        } catch { return cancelled() }
    }

    private func receiveFile(path: String, name: String, share: GuestOpenPolicy.SharedFolder?) async -> HostOpenWire.Response {
        let action = await confirm(.receive(machine: machineName, path: path))
        guard action != .cancel else { return cancelled() }
        var cleanup: URL?
        var retained = false
        var progressWindow: GuestOpenTransferWindow?
        var lease: Int32 = -1
        defer { if lease >= 0 { Darwin.close(lease) } }
        let response: HostOpenWire.Response
        do {
            try check(share)
            await removeOldCopies()
            try check(share)
            let file: URL
            if case .save(let destination) = action {
                file = destination
                guard !FileManager.default.fileExists(atPath: file.path) else { throw FileRPC.Failure.local(EEXIST) }
            } else {
                let directory = scratch.appendingPathComponent(UUID().uuidString, isDirectory: true)
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                                        attributes: [.posixPermissions: 0o700])
                cleanup = directory
                lease = Darwin.open(directory.appendingPathComponent(".lease").path,
                    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0o600)
                guard lease >= 0, flock(lease, LOCK_EX | LOCK_NB) == 0 else { throw FileRPC.Failure.local(errno) }
                let payload = directory.appendingPathComponent("item", isDirectory: true)
                try FileManager.default.createDirectory(at: payload, withIntermediateDirectories: false,
                                                        attributes: [.posixPermissions: 0o700])
                file = payload.appendingPathComponent(GuestOpenPolicy.safeFileName(name))
            }
            let scoped = file.deletingLastPathComponent().startAccessingSecurityScopedResource()
            defer { if scoped { file.deletingLastPathComponent().stopAccessingSecurityScopedResource() } }
            // The shared transport streams and publishes atomically on the
            // target volume. No whole-file allocation or second copy is needed.
            progressWindow = transferProgress == nil ? GuestOpenTransferWindow(name: name, machine: machineName,
                cancel: { [weak self] in self?.active?.cancel() }) : nil
            let window = progressWindow, externalProgress = transferProgress, requestID = activeID
            try await fileAccess.exportFile(URL(fileURLWithPath: path), to: file, progress: { [weak self] progress in
                externalProgress?(progress)
                Task { @MainActor in
                    guard let self, self.activeID == requestID else { return }
                    do { try self.check(share) }
                    catch { self.active?.cancel() }
                    window?.update(progress)
                }
            })
            if case .save = action { cleanup = file }
            try check(share)
            try await quarantine(file)
            try check(share)
            if case .save = action {
                retained = true
                response = .init(status: .opened, message: NPText("Saved on the Mac."))
            } else {
                if try GuestOpenPolicy.mayExecute(file) {
                    guard await confirm(.execute(machine: machineName, file: file)) == .open else { throw CancellationError() }
                    try check(share)
                }
                response = await opened(file)
                retained = response.status == .opened
            }
        } catch is CancellationError { response = cancelled() }
        catch { response = .init(status: .failed, message: NPText("The item could not be received or opened on the Mac: %@", error.localizedDescription)) }
        progressWindow?.close()
        if let cleanup, !retained {
            // Cleanup must finish even if the request was cancelled, while the
            // main actor remains available for windows and other connections.
            await Task.detached(priority: .utility) { try? FileManager.default.removeItem(at: cleanup) }.value
        }
        return response
    }

    private func opened(_ url: URL) async -> HostOpenWire.Response {
        guard !Task.isCancelled, !stopped, isEnabled() else { return cancelled() }
        return await open(url) ? .init(status: .opened)
            : .init(status: .failed, message: NPText("The Mac could not open it."))
    }

    /// Runtime cleanup, with no aggregate size quota that would secretly impose
    /// a single-file limit. Pending/refused/failed copies are removed immediately.
    public func removeOldCopies(olderThan age: TimeInterval = 24 * 3600) async {
        let scratch = scratch
        let cleanup = Task.detached(priority: .utility) {
            let cutoff = Date().addingTimeInterval(-age)
            guard let items = try? FileManager.default.contentsOfDirectory(at: scratch,
                includingPropertiesForKeys: [.contentModificationDateKey]) else { return }
            for item in items {
                guard !Task.isCancelled else { return }
                guard let modified = try? item.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate,
                      modified < cutoff else { continue }
                let lease = Darwin.open(item.appendingPathComponent(".lease").path, O_RDWR | O_CLOEXEC | O_NOFOLLOW)
                guard lease >= 0 else { continue }
                defer { Darwin.close(lease) }
                // An approval/transfer can legitimately last more than a day.
                // flock also protects concurrent owners in another VM process.
                guard flock(lease, LOCK_EX | LOCK_NB) == 0 else { continue }
                try? FileManager.default.removeItem(at: item)
            }
        }
        await withTaskCancellationHandler(operation: { await cleanup.value }, onCancel: { cleanup.cancel() })
    }

    nonisolated public static func markAsDownloaded(_ file: URL) async throws {
        let marking = Task.detached(priority: .utility) {
            var values = URLResourceValues()
            values.quarantineProperties = [kLSQuarantineAgentNameKey as String: "NativePipe",
                kLSQuarantineTypeKey as String: kLSQuarantineTypeOtherDownload as String]
            func mark(_ file: URL) throws {
                try Task.checkCancellation()
                guard try file.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink != true else {
                    throw FileRPC.Failure.invalidPath
                }
                let attributes = try FileManager.default.attributesOfItem(atPath: file.path)
                let mode = attributes[.posixPermissions] as? Int ?? 0o600
                // Preserve a downloaded read-only item's mode after marking.
                try FileManager.default.setAttributes([.posixPermissions: mode | 0o200], ofItemAtPath: file.path)
                defer { try? FileManager.default.setAttributes([.posixPermissions: mode], ofItemAtPath: file.path) }
                var item = file
                try item.setResourceValues(values)
            }
            try mark(file)
            if try file.resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true,
               let items = FileManager.default.enumerator(at: file, includingPropertiesForKeys: [.isSymbolicLinkKey]) {
                while let child = items.nextObject() as? URL { try mark(child) }
            }
        }
        try await withTaskCancellationHandler(operation: { try await marking.value }, onCancel: { marking.cancel() })
    }

    public static func openWithWorkspace(_ url: URL) async -> Bool {
        guard !Task.isCancelled else { return false }
        return await withCheckedContinuation { continuation in
            let configuration = NSWorkspace.OpenConfiguration()
            configuration.activates = true
            NSWorkspace.shared.open(url, configuration: configuration) { _, error in
                continuation.resume(returning: error == nil)
            }
        }
    }
}
