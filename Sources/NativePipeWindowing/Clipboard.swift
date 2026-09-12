import AppKit
import NativePipeProtocol

/// Joins the Wayland selection to NSPasteboard.
///
/// The two models agree more than they differ: both are "one owner, advertising
/// a set of types, handing over bytes on demand". What they disagree about is
/// naming. Small values and file names are resolved at announce time; file
/// contents are fetched only by a paste request through ClipboardFileBroker.
@MainActor
final class ClipboardBridge {
    /// Sends a host command to the guest.
    var output: ((Windowing.HostCommand) -> Void)?
    var fileAccess: (any UserFileAccess)?
    var onError: ((Error) -> Void)?
    private let fileBroker: ClipboardFileBroker
    private var selectionGeneration: UInt64 = 0
    private var connectionGeneration: UInt64 = 0
    private var hostTransfers: [UInt32: Task<Void, Never>] = [:]

    private let pasteboard: NSPasteboard
    private var connected = false
    private var nextToken: UInt32 = 0
    /// Pastes from the guest that are waiting on `hostSelectionRequest`.
    private var pendingGuestReads: [UInt32: (Data?) -> Void] = [:]
    /// The change count as of the last time *we* wrote, so the poll below can
    /// tell the Mac's own copies apart from the echo of a guest's.
    private var lastSeenChangeCount: Int
    private var poll: Timer?
    private var allowsHostToGuest = true
    private var allowsGuestToHost = true

    /// Preference order, best first. The guest is asked for the first type it
    /// actually offers rather than for everything, because the announcement is
    /// resolved eagerly: fetching all of them would copy an image across the
    /// channel to satisfy a paste that only ever wanted the text.
    private static let guestToNative: [(mime: String, native: NSPasteboard.PasteboardType)] = [
        ("text/uri-list", .fileURL),
        ("text/plain;charset=utf-8", .string),
        ("UTF8_STRING", .string),
        ("text/plain", .string),
        ("STRING", .string),
        ("TEXT", .string),
        ("text/html", .html),
        ("image/png", .png),
        ("image/tiff", .tiff),
    ]

    private static func note(_ message: @autoclosure () -> String) {
        guard ProcessInfo.processInfo.environment["NATIVEPIPE_WINDOW_TRACE"] != nil else { return }
        FileHandle.standardError.write(Data("[clip] \(message())\n".utf8))
    }

    init(pasteboard: NSPasteboard = .general, fileDirectory: URL? = nil) {
        self.pasteboard = pasteboard
        fileBroker = ClipboardFileBroker(directory: fileDirectory ?? ClipboardFileBroker.defaultDirectory)
        lastSeenChangeCount = pasteboard.changeCount
    }

    func connectionReady() {
        disconnect()
        connected = true
        pollPasteboard(force: true)
    }

    func start() {
        guard poll == nil else { return }
        // NSPasteboard has no change notification, so the change count is the
        // only signal there is. Reading it is a cheap round trip to pboard and
        // nothing else happens unless the number moved.
        let timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.pollPasteboard() }
        }
        RunLoop.main.add(timer, forMode: .common)
        poll = timer
    }

    func stop() {
        poll?.invalidate()
        poll = nil
        pendingGuestReads.removeAll()
        disconnect()
    }

    func disconnect() {
        connected = false
        connectionGeneration &+= 1
        selectionGeneration &+= 1
        fileBroker.stop()
        for task in hostTransfers.values { task.cancel() }
        hostTransfers.removeAll()
        pendingGuestReads.removeAll()
    }

    func setPolicy(hostToGuest: Bool, guestToHost: Bool) {
        let hostChanged = allowsHostToGuest != hostToGuest
        allowsHostToGuest = hostToGuest
        allowsGuestToHost = guestToHost
        if hostToGuest {
            start()
            if hostChanged { pollPasteboard(force: true) }
        } else {
            poll?.invalidate()
            poll = nil
            if hostChanged, connected { output?(.hostSelectionOffered(mimeTypes: [])) }
            for task in hostTransfers.values { task.cancel() }
        }
        if !guestToHost {
            fileBroker.stop()
            let pending = Array(pendingGuestReads.values)
            pendingGuestReads.removeAll()
            for completion in pending { completion(nil) }
        }
    }

    // MARK: - Guest owns the selection

    /// A guest client took the selection. Fetch the best type it offers and put
    /// it on the Mac's pasteboard.
    func guestOffered(mimeTypes: [String]) {
        selectionGeneration &+= 1
        pendingGuestReads.removeAll()
        fileBroker.revoke()
        let generation = selectionGeneration
        let changeCount = pasteboard.changeCount
        guard connected, allowsGuestToHost else { return }
        guard !mimeTypes.isEmpty else { return }
        let offered = Set(mimeTypes)
        guard let match = Self.guestToNative.first(where: { offered.contains($0.mime) }) else {
            return
        }
        requestFromGuest(mime: match.mime) { [weak self] data in
            guard let self, self.selectionGeneration == generation,
                  self.pasteboard.changeCount == changeCount,
                  let data, !data.isEmpty else { return }
            if match.native == .fileURL {
                guard let access = self.fileAccess, let urls = try? FileTransferURLs.decode(data) else { return }
                do { try self.fileBroker.publish(urls, using: access, to: self.pasteboard) }
                catch { self.onError?(error); return }
            } else {
                // declareTypes, not clearContents: setData refuses to write a
                // type the pasteboard was never told to expect, and it reports
                // that by returning false rather than by failing loudly. Both
                // halves of the transfer can be working and the clipboard still
                // come up empty.
                self.pasteboard.declareTypes([match.native], owner: nil)
                if !self.pasteboard.setData(data, forType: match.native) {
                    Self.note("pasteboard refused \(match.native.rawValue)")
                    return
                }
            }
            // Our own write must not read back as the Mac taking the clipboard.
            self.lastSeenChangeCount = self.pasteboard.changeCount
        }
    }

    private func requestFromGuest(mime: String, completion: @escaping (Data?) -> Void) {
        guard allowsGuestToHost else { completion(nil); return }
        nextToken &+= 1
        let token = nextToken
        pendingGuestReads[token] = completion
        output?(.selectionRequest(token: token, mimeType: mime))
    }

    /// The guest answered a `selectionRequest`.
    func guestSuppliedData(token: UInt32, data: Data?) {
        guard let completion = pendingGuestReads.removeValue(forKey: token) else { return }
        completion(data)
    }

    // MARK: - Mac owns the selection

    func pollPasteboard(force: Bool = false) {
        guard connected, allowsHostToGuest else { return }
        let current = pasteboard.changeCount
        guard force || current != lastSeenChangeCount else { return }
        lastSeenChangeCount = current
        selectionGeneration &+= 1
        pendingGuestReads.removeAll()
        fileBroker.revoke()

        var mimeTypes: [String] = []
        let available = Set(pasteboard.types ?? [])
        if ClipboardFileBroker.offer(from: pasteboard) != nil { mimeTypes.append("text/uri-list") }
        for entry in Self.guestToNative where available.contains(entry.native) {
            if !mimeTypes.contains(entry.mime) { mimeTypes.append(entry.mime) }
        }
        output?(.hostSelectionOffered(mimeTypes: mimeTypes))
    }

    /// A guest client is pasting and wants the Mac's clipboard in `mimeType`.
    func guestRequestedHostData(token: UInt32, mimeType: String) {
        guard connected, allowsHostToGuest else {
            output?(.hostSelectionData(token: token, mimeType: mimeType, data: nil))
            return
        }
        let native = Self.guestToNative.first { $0.mime == mimeType }?.native
        if native == .fileURL {
            let offer = ClipboardFileBroker.offer(from: pasteboard)
            let urls = pasteboard.readObjects(forClasses: [NSURL.self],
                options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
            let access = fileAccess
            let connection = connectionGeneration
            let broker = fileBroker
            // A request token identifies one live paste, including while its
            // importer is suspended. Duplicate tokens cannot replace its owner.
            guard hostTransfers[token] == nil else { return }
            guard hostTransfers.count < 4 else {
                output?(.hostSelectionData(token: token, mimeType: mimeType, data: nil))
                return
            }
            hostTransfers[token] = Task { @MainActor [weak self] in
                var bytes: Data?
                do {
                    if let access, let offer {
                        let receipt = try await broker.receive(offer)
                        defer { withExtendedLifetime(receipt) {} }
                        bytes = FileTransferURLs.encode(try await access.importFiles(receipt.urls, shareDirectories: false))
                    } else if let access, !urls.isEmpty {
                        bytes = FileTransferURLs.encode(try await access.importFiles(urls))
                    }
                    try Task.checkCancellation()
                } catch {
                    bytes = nil
                    if !(error is CancellationError), let self, self.connectionGeneration == connection {
                        self.onError?(error)
                    }
                }
                guard let self, self.connectionGeneration == connection else { return }
                self.hostTransfers[token] = nil
                self.output?(.hostSelectionData(token: token, mimeType: mimeType,
                    data: self.allowsHostToGuest ? bytes : nil))
            }
            return
        }
        var data: Data?
        if let native {
            data = pasteboard.data(forType: native)
        }
        if let bytes = data, bytes.count > WindowWire.maximumClipboardDataSize {
            Self.note("refusing \(bytes.count)-byte host selection")
            data = nil
        }
        // Answering with nil rather than staying silent matters: the guest has a
        // client blocked on a pipe, and it closes that pipe when the answer
        // arrives. No answer would leave the paste hanging forever.
        output?(.hostSelectionData(
            token: token, mimeType: mimeType,
            data: data))
    }
}
