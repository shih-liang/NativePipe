import AppKit
import NativePipeProtocol

/// Clipboard file URLs for guest files when there is no live shared volume.
///
/// File promises exist only for drags; the copy-and-paste pasteboard supplies
/// data on demand through NSPasteboardItemDataProvider instead. Each guest file
/// becomes one pasteboard item advertising `public.file-url`; the file is
/// copied into the session's cache only when an app actually pastes it.
///
/// AppKit asks for the data synchronously. The copy therefore runs off the
/// main actor where the connection allows it (`detachedExport`), so the wait
/// works whichever thread or queue AppKit calls from.
public final class GuestFilePasteboardProvider: NSObject, NSPasteboardItemDataProvider, @unchecked Sendable {
    private struct Entry { let remote: URL; var copy: URL? }
    private let lock = NSLock()
    private var entries: [ObjectIdentifier: Entry] = [:]
    private let folder: URL
    private let export: (@Sendable (URL, URL) async throws -> Void)?
    private weak var session: SharingSession?
    /// Called (on the main actor) when a paste could not be supplied.
    public var onError: (@MainActor (Error) -> Void)?

    @MainActor
    public init(session: SharingSession) throws {
        self.session = session
        folder = try session.reserveCopyFolder(purpose: .clipboard)
        export = session.transport.detachedExport
    }

    /// Pasteboard items for the guest files, in order.
    @MainActor
    public func items(for guestFiles: [URL]) -> [NSPasteboardItem] {
        guestFiles.map { remote in
            let item = NSPasteboardItem()
            item.setDataProvider(self, forTypes: [.fileURL])
            lock.withLock { entries[ObjectIdentifier(item)] = Entry(remote: remote) }
            return item
        }
    }

    public func pasteboard(_ pasteboard: NSPasteboard?, item: NSPasteboardItem,
                           provideDataForType type: NSPasteboard.PasteboardType) {
        let key = ObjectIdentifier(item)
        guard type == .fileURL, let entry = lock.withLock({ entries[key] }) else { return }
        if let copy = entry.copy ?? copy(entry.remote) {
            lock.withLock { entries[key]?.copy = copy }
            item.setString(copy.absoluteString, forType: .fileURL)
        }
    }

    public func pasteboardFinishedWithDataProvider(_ pasteboard: NSPasteboard) {}

    private func copy(_ remote: URL) -> URL? {
        let name = remote.lastPathComponent
        guard !["", "/", ".", ".."].contains(name) else { return nil }
        // Each item gets its own subfolder: two guest files may share a name.
        let destination = folder.appendingPathComponent(UUID().uuidString, isDirectory: true)
            .appendingPathComponent(name, isDirectory: remote.hasDirectoryPath)
        var failure: Error?
        if let export {
            let done = DispatchSemaphore(value: 0)
            Task.detached {
                do {
                    try FileManager.default.createDirectory(at: destination.deletingLastPathComponent(),
                                                            withIntermediateDirectories: false)
                    try await export(remote, destination)
                } catch { failure = error }
                done.signal()
            }
            done.wait()
        } else if Thread.isMainThread {
            // This connection copies on the main actor: keep the main run loop
            // turning while the paste waits, as AppKit's modal waits do.
            MainActor.assumeIsolated {
                var finished = false
                let session = session
                Task { @MainActor in
                    do {
                        guard let session else { throw CancellationError() }
                        let copied = try await session.temporaryCopy(of: remote, purpose: .clipboard)
                        try FileManager.default.createDirectory(at: destination.deletingLastPathComponent(),
                                                                withIntermediateDirectories: false)
                        try FileManager.default.moveItem(at: copied, to: destination)
                    } catch { failure = error }
                    finished = true
                }
                while !finished { _ = RunLoop.current.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05)) }
            }
        } else {
            failure = CancellationError()
        }
        if let failure {
            if !(failure is CancellationError) {
                let report = onError
                DispatchQueue.main.async { MainActor.assumeIsolated { report?(failure) } }
            }
            return nil
        }
        return destination
    }
}
