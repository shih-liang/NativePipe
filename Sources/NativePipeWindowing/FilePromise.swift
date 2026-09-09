import AppKit
import NativePipeProtocol
import UniformTypeIdentifiers

// Published by the task before signalling, read only after the worker waits.
private final class PromiseResult: @unchecked Sendable { var error: Error? }

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

/// Shared by clipboard and drag sources. Finder chooses the destination; no
/// guest path is ever published as a supposedly local file URL.
@MainActor
final class LinuxFilePromise: NSObject, NSFilePromiseProviderDelegate {
    let remote: URL
    let access: any UserFileAccess
    var completed: ((Error?) -> Void)?
    lazy var provider: NSFilePromiseProvider = {
        let type = remote.hasDirectoryPath ? UTType.folder : (UTType(filenameExtension: remote.pathExtension) ?? .data)
        return NSFilePromiseProvider(fileType: type.identifier, delegate: self)
    }()
    nonisolated private static let queue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "com.nativepipe.file-promises"
        queue.maxConcurrentOperationCount = 4
        return queue
    }()

    init(remote: URL, access: any UserFileAccess) { self.remote = remote; self.access = access }

    func filePromiseProvider(_ filePromiseProvider: NSFilePromiseProvider, fileNameForType fileType: String) -> String {
        remote.lastPathComponent
    }
    nonisolated func operationQueue(for filePromiseProvider: NSFilePromiseProvider) -> OperationQueue { Self.queue }

    nonisolated func filePromiseProvider(_ filePromiseProvider: NSFilePromiseProvider,
        writePromiseTo url: URL, completionHandler: @escaping @Sendable (Error?) -> Void) {
        let result = PromiseResult()
        let done = DispatchSemaphore(value: 0)
        // AppKit already coordinates this write. A new coordinator here waits
        // for AppKit's claim, which cannot finish until this delegate returns.
        Task { @MainActor in
            do { try await self.access.exportFile(self.remote, to: url) }
            catch { result.error = error }
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
