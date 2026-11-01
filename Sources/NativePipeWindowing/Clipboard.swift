import AppKit
import NativePipeProtocol
import NativePipeSharing

/// Joins the Wayland selection to NSPasteboard.
///
/// The two models agree more than they differ: both are "one owner, advertising
/// a set of types, handing over bytes on demand". What they disagree about is
/// naming, which ContentFormats translates. Small values and file names are
/// resolved at announce time; files go through the machine's SharingSession,
/// which either mounts them or copies each one when it is pasted.
@MainActor
final class ClipboardBridge {
    /// Sends a host command to the guest.
    var output: ((Windowing.HostCommand) -> Void)?
    var sharing: SharingSession?
    var onError: ((Error) -> Void)?
    /// Supplies on-demand file copies while it owns the Mac pasteboard.
    private var fileProvider: GuestFilePasteboardProvider?
    private var selectionGeneration: UInt64 = 0
    private var connectionGeneration: UInt64 = 0
    private var hostTransfers: [UInt32: Task<Void, Never>] = [:]
    private var guestPublication: Task<Void, Never>?

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

    private static func note(_ message: @autoclosure () -> String) {
        guard ProcessInfo.processInfo.environment["NATIVEPIPE_WINDOW_TRACE"] != nil else { return }
        FileHandle.standardError.write(Data("[clip] \(message())\n".utf8))
    }

    init(pasteboard: NSPasteboard = .general) {
        self.pasteboard = pasteboard
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
        abandonGuestReads()
        disconnect()
    }

    func disconnect() {
        connected = false
        connectionGeneration &+= 1
        selectionGeneration &+= 1
        guestPublication?.cancel(); guestPublication = nil
        for task in hostTransfers.values { task.cancel() }
        hostTransfers.removeAll()
        abandonGuestReads()
    }

    func setPolicy(hostToGuest: Bool, guestToHost: Bool) {
        let hostChanged = allowsHostToGuest != hostToGuest
        let guestChanged = allowsGuestToHost != guestToHost
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
            if guestChanged { sharing?.revoke(.clipboard) }
            selectionGeneration &+= 1
            guestPublication?.cancel(); guestPublication = nil
            abandonGuestReads()
        }
    }

    // MARK: - Guest owns the selection

    /// A guest client took the selection. Fetch the best type it offers and put
    /// it on the Mac's pasteboard.
    func guestOffered(mimeTypes: [String]) {
        selectionGeneration &+= 1
        guestPublication?.cancel(); guestPublication = nil
        abandonGuestReads()
        sharing?.revoke(.clipboard)
        let generation = selectionGeneration
        let changeCount = pasteboard.changeCount
        guard connected, allowsGuestToHost else { return }
        // Only the best representations are fetched: the announcement is
        // resolved eagerly, and fetching every type would copy an image across
        // the channel for a paste that only ever wanted the text.
        let wanted = ContentFormats.preferredGuestTypes(from: mimeTypes)
        guard !wanted.isEmpty else { return }
        var answers = [Data?](repeating: nil, count: wanted.count)
        var remaining = wanted.count
        for (index, mime) in wanted.enumerated() {
            requestFromGuest(mime: mime) { [weak self] data in
                answers[index] = data
                remaining -= 1
                guard remaining == 0, let self, self.selectionGeneration == generation,
                      self.pasteboard.changeCount == changeCount else { return }
                let fetched = zip(wanted, answers).compactMap { mime, data in
                    data.flatMap { $0.isEmpty ? nil : (mime: mime, data: $0) }
                }
                guard !fetched.isEmpty else { return }
                self.publishGuestSelection(fetched, generation: generation, changeCount: changeCount)
            }
        }
    }

    /// Puts what the guest copied on the Mac pasteboard as one item. Files go
    /// through the sharing backend; everything else is converted right away.
    private func publishGuestSelection(_ fetched: [(mime: String, data: Data)],
                                       generation: UInt64, changeCount: Int) {
        if let files = fetched.first(where: { $0.mime == ContentFormats.fileList }),
           let urls = try? FileTransferURLs.decode(files.data), urls.allSatisfy(\.isFileURL) {
            guard let sharing else { onError?(SharingUnavailable()); return }
            guestPublication = Task { @MainActor [weak self] in
                do {
                    let shared = try await sharing.hostFiles(for: urls, purpose: .clipboard)
                    try Task.checkCancellation()
                    guard let self, self.connected, self.allowsGuestToHost, self.selectionGeneration == generation,
                          self.pasteboard.changeCount == changeCount else { return }
                    self.pasteboard.clearContents()
                    switch shared {
                    case .mounted(let hostURLs):
                        guard self.pasteboard.writeObjects(hostURLs as [NSURL]) else { throw FileRPC.Failure.protocolError }
                    case .deferred(let guestURLs):
                        // Copied only when an app pastes them (copy at paste time).
                        let provider = try GuestFilePasteboardProvider(session: sharing)
                        provider.onError = { [weak self] in self?.onError?($0) }
                        self.fileProvider = provider
                        guard self.pasteboard.writeObjects(provider.items(for: guestURLs)) else { throw FileRPC.Failure.protocolError }
                    }
                    // Our own write must not read back as the Mac taking the clipboard.
                    self.lastSeenChangeCount = self.pasteboard.changeCount
                } catch {
                    guard !(error is CancellationError), let self, self.selectionGeneration == generation else { return }
                    self.onError?(error)
                }
            }
            return
        }
        let item = NSPasteboardItem()
        var written = false
        for entry in fetched {
            if entry.data.count > WindowWire.maximumClipboardDataSize {
                Self.note("refusing \(entry.data.count)-byte guest selection")
                continue
            }
            written = ContentFormats.write(entry.data, mime: entry.mime, to: item) || written
        }
        guard written else { Self.note("no Mac representation for \(fetched.map(\.mime))"); return }
        pasteboard.clearContents()
        guard pasteboard.writeObjects([item]) else { Self.note("pasteboard refused the guest selection"); return }
        // Our own write must not read back as the Mac taking the clipboard.
        lastSeenChangeCount = pasteboard.changeCount
    }

    /// Every waiting read is answered, so no fetch is left suspended.
    private func abandonGuestReads() {
        let pending = Array(pendingGuestReads.values)
        pendingGuestReads.removeAll()
        for completion in pending { completion(nil) }
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
        guestPublication?.cancel(); guestPublication = nil
        abandonGuestReads()
        sharing?.revoke(.clipboard)
        fileProvider = nil
        output?(.hostSelectionOffered(mimeTypes: ContentFormats.mimeTypes(for: pasteboard)))
    }

    /// A guest client is pasting and wants the Mac's clipboard in `mimeType`.
    func guestRequestedHostData(token: UInt32, mimeType: String) {
        guard connected, allowsHostToGuest else {
            output?(.hostSelectionData(token: token, mimeType: mimeType, data: nil))
            return
        }
        if mimeType == ContentFormats.fileList, ContentFormats.hasFiles(pasteboard) {
            let urls = pasteboard.readObjects(forClasses: [NSURL.self],
                options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
            let sharing = sharing
            let connection = connectionGeneration
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
                    if let sharing, !urls.isEmpty {
                        bytes = FileTransferURLs.encode(try await sharing.guestURLs(for: urls, purpose: .clipboard))
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
        var data = ContentFormats.data(forMIME: mimeType, from: pasteboard)
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
