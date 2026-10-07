import AppKit
import Darwin
import NativePipeProtocol
import UniformTypeIdentifiers

// Published by the task before signalling, read only after the worker waits.
private final class PromiseResult: @unchecked Sendable { var error: Error? }
/// AppKit chooses one representation per pasteboard item. Prefer promises over
/// URLs when an item offers both, while retaining ordinary files in mixed drops.
/// Start receivers synchronously, then keep their staging alive through import.
@MainActor
struct IncomingDragFiles {
    private let urls: [URL]
    private let receipt: IncomingFilePromises?

    init(_ pasteboard: NSPasteboard) throws {
        let items = pasteboard.readObjects(forClasses: [NSFilePromiseReceiver.self, NSURL.self],
            options: [.urlReadingFileURLsOnly: true]) ?? []
        urls = items.compactMap { ($0 as? NSURL).map { $0 as URL } }
        let receivers = items.compactMap { $0 as? NSFilePromiseReceiver }
        receipt = receivers.isEmpty ? nil : try IncomingFilePromises(receivers)
    }

    func importFiles(using access: any UserFileAccess) async throws -> [URL] {
        defer { withExtendedLifetime(receipt) {} }
        var imported: [URL] = []
        if !urls.isEmpty { imported = try await access.importFiles(urls) }
        if let receipt {
            let files = try await receipt.files()
            // A temporary promised directory must not become a persistent share.
            imported += try await access.importFiles(files, shareDirectories: false)
        }
        try Task.checkCancellation()
        return imported
    }
}

/// Register every receiver synchronously inside performDragOperation. AppKit
/// populates fileNames during registration and requires one shared destination.
@MainActor
final class IncomingFilePromises {
    let directory: URL
    private let stream: AsyncThrowingStream<URL, Error>
    private var remaining = 0
    private var firstError: Error?

    init(_ receivers: [NSFilePromiseReceiver]) throws {
        directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("nativepipe-drop-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        let (stream, continuation) = AsyncThrowingStream<URL, Error>.makeStream()
        self.stream = stream
        for receiver in receivers {
            receiver.receivePromisedFiles(atDestination: directory, options: [:], operationQueue: .main) { [self] url, error in
                MainActor.assumeIsolated {
                    if let error { firstError = firstError ?? error }
                    else { continuation.yield(url) }
                    remaining -= 1
                    if remaining == 0 { continuation.finish(throwing: firstError) }
                }
            }
            remaining += max(1, receiver.fileNames.count)
        }
        if receivers.isEmpty { continuation.finish() }
    }

    func files() async throws -> [URL] {
        var files: [URL] = []
        for try await file in stream { files.append(file) }
        try Task.checkCancellation()
        return files
    }

    // Reader callbacks also retain this object: cancellation must not remove
    // staging while an external app is still writing its promised files.
    deinit { try? FileManager.default.removeItem(at: directory) }
}

/// Drag source adapter. Finder chooses the destination; no
/// guest path is ever published as a supposedly local file URL.
@MainActor
public final class LinuxFilePromise: NSObject, NSFilePromiseProviderDelegate {
    let remote: URL
    private let write: @MainActor (URL) async throws -> Void
    private var transfers: [UUID: Task<Void, Never>] = [:]
    private var cancelled = false
    public var completed: ((Error?) -> Void)?
    private weak var cachedProvider: NSFilePromiseProvider?
    public var provider: NSFilePromiseProvider {
        if let cachedProvider { return cachedProvider }
        let type = remote.hasDirectoryPath ? UTType.folder : (UTType(filenameExtension: remote.pathExtension) ?? .data)
        let provider = NSFilePromiseProvider(fileType: type.identifier, delegate: self)
        // AppKit keeps the provider after the source view disappears. Its
        // delegate is weak; userInfo owns the writer until the promise ends.
        provider.userInfo = self
        cachedProvider = provider
        return provider
    }
    nonisolated private static let queue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "com.nativepipe.file-promises"
        queue.maxConcurrentOperationCount = 4
        return queue
    }()

    public init(remote: URL, write: @escaping @MainActor (URL) async throws -> Void) {
        self.remote = remote; self.write = write
    }

    public convenience init(remote: URL, access: any UserFileAccess, publishGuestFiles: GuestFilePublisher? = nil) {
        self.init(remote: remote) { destination in
            guard let publish = publishGuestFiles else { throw GuestFileSharingError.unavailable }
            let files = try await publish([remote], access, .drag)
            try Task.checkCancellation()
            guard files.count == 1, let source = files.first else { throw FileRPC.Failure.protocolError }
            _ = try FileTransferURLs.decode(FileTransferURLs.encode(files))
            try await FileTransferLocalIO.copy(source: source, to: destination)
        }
    }

    func cancel() { cancelled = true; transfers.values.forEach { $0.cancel() } }

    public func filePromiseProvider(_ filePromiseProvider: NSFilePromiseProvider, fileNameForType fileType: String) -> String {
        remote.lastPathComponent
    }
    nonisolated public func operationQueue(for filePromiseProvider: NSFilePromiseProvider) -> OperationQueue { Self.queue }

    nonisolated public func filePromiseProvider(_ filePromiseProvider: NSFilePromiseProvider,
        writePromiseTo url: URL, completionHandler: @escaping @Sendable (Error?) -> Void) {
        let result = PromiseResult()
        let done = DispatchSemaphore(value: 0)
        // AppKit already coordinates this write. A new coordinator here waits
        // for AppKit's claim, which cannot finish until this delegate returns.
        Task { @MainActor in
            let id = UUID()
            let transfer = Task { @MainActor in
                do {
                    try Task.checkCancellation()
                    try await self.write(url)
                } catch { result.error = error }
            }
            self.transfers[id] = transfer
            if self.cancelled { transfer.cancel() }
            await transfer.value
            self.transfers[id] = nil
            done.signal()
        }
        // Keep AppKit's claim alive until the async transfer completes. Only
        // the file-promise worker waits, never the AppKit event loop.
        done.wait()
        let error = result.error
        completionHandler(error)
        Task { @MainActor in self.completed?(error) }
    }

}
