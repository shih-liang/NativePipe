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
/// All guest files are published through the virtual filesystem; explicit
/// Save copies the mounted item to the user-selected destination.
@MainActor
public final class GuestOpenCoordinator {
    private let machineName: String
    private let isEnabled: () -> Bool
    private let shares: () -> [GuestOpenPolicy.SharedFolder]
    private let fileAccess: UserFileAccess
    private let confirm: @MainActor (GuestOpenPrompt) async -> GuestOpenAction
    private let open: @MainActor (URL) async -> Bool
    private let quarantine: @MainActor (URL) async throws -> Void
    private let transferProgress: (@Sendable (FileTransferProgress) -> Void)?
    private let publishGuestFiles: GuestFilePublisher?
    private var active: Task<HostOpenWire.Response, Never>?
    private var activeID: UUID?
    private var stopped = false
    private var nextRequest: TimeInterval = 0

    public init(machineName: String, isEnabled: @escaping () -> Bool = { true },
                shares: @escaping () -> [GuestOpenPolicy.SharedFolder] = { [] },
                fileAccess: UserFileAccess,
                confirm: @escaping @MainActor (GuestOpenPrompt) async -> GuestOpenAction = GuestOpenDialogs.confirm,
                open: @escaping @MainActor (URL) async -> Bool = GuestOpenCoordinator.openWithWorkspace,
                quarantine: @escaping @MainActor (URL) async throws -> Void = GuestOpenCoordinator.markAsDownloaded,
                transferProgress: (@Sendable (FileTransferProgress) -> Void)? = nil,
                publishGuestFiles: GuestFilePublisher? = nil) {
        self.machineName = machineName; self.isEnabled = isEnabled; self.shares = shares
        self.fileAccess = fileAccess; self.confirm = confirm
        self.open = open; self.quarantine = quarantine
        self.transferProgress = transferProgress
        self.publishGuestFiles = publishGuestFiles
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
        var savedCopy: URL?
        var scopedDestination: URL?
        var progressWindow: GuestOpenTransferWindow?
        defer { progressWindow?.close(); scopedDestination?.stopAccessingSecurityScopedResource() }
        do {
            try check(share)
            guard let publishGuestFiles else { throw GuestFileSharingError.unavailable }
            // Consent precedes registration and all range reads. Opening does
            // not crawl or mutate the read-only volume's quarantine attributes.
            let files = try await publishGuestFiles([RemoteFileURL.make(path)], fileAccess, .open)
            try check(share)
            guard files.count == 1, let source = files.first else { throw FileRPC.Failure.protocolError }
            _ = try FileTransferURLs.decode(FileTransferURLs.encode(files))
            if case .save(let destination) = action {
                let parent = destination.deletingLastPathComponent()
                if parent.startAccessingSecurityScopedResource() { scopedDestination = parent }
                progressWindow = transferProgress == nil ? GuestOpenTransferWindow(name: name, machine: machineName,
                    cancel: { [weak self] in self?.active?.cancel() }) : nil
                let window = progressWindow, externalProgress = transferProgress, requestID = activeID
                try await FileTransferLocalIO.copy(source: source, to: destination) { [weak self] progress in
                    externalProgress?(progress)
                    Task { @MainActor in
                        guard let self, self.activeID == requestID else { return }
                        do { try self.check(share) }
                        catch { self.active?.cancel() }
                        window?.update(progress)
                    }
                }
                savedCopy = destination
                try check(share)
                try await quarantine(destination)
                try check(share)
                return .init(status: .opened, message: NPText("Saved on the Mac."))
            }
            let executable = try await inspectExecutionRisk(source)
            try check(share)
            if executable {
                guard await confirm(.execute(machine: machineName, file: source)) == .open else { return cancelled() }
                try check(share)
            }
            return await opened(source)
        } catch {
            if let savedCopy {
                // Only our successfully published destination is removed. A
                // rejected existing destination must remain untouched.
                await FileTransferLocalIO.removeFailedCopy(at: savedCopy)
            }
            if error is CancellationError { return cancelled() }
            return .init(status: .failed, message: NPText("The item could not be received or opened on the Mac: %@", error.localizedDescription))
        }
    }

    private func opened(_ url: URL) async -> HostOpenWire.Response {
        guard !Task.isCancelled, !stopped, isEnabled() else { return cancelled() }
        return await open(url) ? .init(status: .opened)
            : .init(status: .failed, message: NPText("The Mac could not open it."))
    }

    private func inspectExecutionRisk(_ file: URL) async throws -> Bool {
        // FSKit may ask this same process's main actor for metadata or four
        // magic bytes. Synchronous Foundation IO must not block its owner.
        return try await FileTransferLocalIO.perform {
            try GuestOpenPolicy.mayExecute(file)
        }
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
