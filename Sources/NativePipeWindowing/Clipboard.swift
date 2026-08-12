import AppKit
import NativePipeProtocol

/// Joins the Wayland selection to NSPasteboard.
///
/// The two models agree more than they differ: both are "one owner, advertising
/// a set of types, handing over bytes on demand". What they disagree about is
/// naming, and who is allowed to be lazy. NSPasteboard lets an owner promise
/// data and supply it later, but only through a synchronous callback — and the
/// data lives on the far side of a vsock round trip, so that callback cannot be
/// honoured without blocking AppKit. So the guest's side of the bridge is
/// resolved eagerly at announce time, and only the host's side stays lazy.
@MainActor
final class ClipboardBridge {
    /// Sends a host command to the guest.
    var output: ((Windowing.HostCommand) -> Void)?

    private let pasteboard = NSPasteboard.general
    private var nextToken: UInt32 = 0
    /// Pastes from the guest that are waiting on `hostSelectionRequest`.
    private var pendingGuestReads: [UInt32: (Data?) -> Void] = [:]
    /// The change count as of the last time *we* wrote, so the poll below can
    /// tell the Mac's own copies apart from the echo of a guest's.
    private var lastSeenChangeCount: Int
    private var poll: Timer?

    /// Preference order, best first. The guest is asked for the first type it
    /// actually offers rather than for everything, because the announcement is
    /// resolved eagerly: fetching all of them would copy an image across the
    /// channel to satisfy a paste that only ever wanted the text.
    private static let guestToNative: [(mime: String, native: NSPasteboard.PasteboardType)] = [
        ("text/plain;charset=utf-8", .string),
        ("UTF8_STRING", .string),
        ("text/plain", .string),
        ("STRING", .string),
        ("TEXT", .string),
        ("text/html", .html),
        ("image/png", .png),
        ("image/tiff", .tiff),
        ("text/uri-list", .fileURL),
    ]

    private static func note(_ message: @autoclosure () -> String) {
        guard ProcessInfo.processInfo.environment["NATIVEPIPE_WINDOW_TRACE"] != nil else { return }
        FileHandle.standardError.write(Data("[clip] \(message())\n".utf8))
    }

    init() {
        lastSeenChangeCount = pasteboard.changeCount
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
    }

    // MARK: - Guest owns the selection

    /// A guest client took the selection. Fetch the best type it offers and put
    /// it on the Mac's pasteboard.
    func guestOffered(mimeTypes: [String]) {
        guard !mimeTypes.isEmpty else { return }
        let offered = Set(mimeTypes)
        guard let match = Self.guestToNative.first(where: { offered.contains($0.mime) }) else {
            return
        }
        requestFromGuest(mime: match.mime) { [weak self] data in
            guard let self, let data, !data.isEmpty else { return }
            if match.native == .fileURL {
                self.pasteboard.clearContents()
                // text/uri-list is a newline-separated list with #-comments; the
                // Mac wants real URL items.
                let urls = String(decoding: data, as: UTF8.self)
                    .split(whereSeparator: \.isNewline)
                    .filter { !$0.hasPrefix("#") }
                    .compactMap { URL(string: String($0)) }
                if urls.isEmpty { return }
                self.pasteboard.writeObjects(urls as [NSURL])
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
        nextToken &+= 1
        let token = nextToken
        pendingGuestReads[token] = completion
        output?(.selectionRequest(token: token, mimeType: mime))
    }

    /// The guest answered a `selectionRequest`.
    func guestSuppliedData(token: UInt32, base64: String?) {
        guard let completion = pendingGuestReads.removeValue(forKey: token) else { return }
        completion(base64.flatMap { Data(base64Encoded: $0) })
    }

    // MARK: - Mac owns the selection

    private func pollPasteboard() {
        let current = pasteboard.changeCount
        guard current != lastSeenChangeCount else { return }
        lastSeenChangeCount = current

        var mimeTypes: [String] = []
        let available = Set(pasteboard.types ?? [])
        for entry in Self.guestToNative where available.contains(entry.native) {
            if !mimeTypes.contains(entry.mime) { mimeTypes.append(entry.mime) }
        }
        guard !mimeTypes.isEmpty else { return }
        output?(.hostSelectionOffered(mimeTypes: mimeTypes))
    }

    /// A guest client is pasting and wants the Mac's clipboard in `mimeType`.
    func guestRequestedHostData(token: UInt32, mimeType: String) {
        let native = Self.guestToNative.first { $0.mime == mimeType }?.native
        var data: Data?
        if let native {
            if native == .fileURL {
                let urls = pasteboard.readObjects(forClasses: [NSURL.self]) as? [URL] ?? []
                if !urls.isEmpty {
                    data = Data((urls.map(\.absoluteString).joined(separator: "\r\n") + "\r\n").utf8)
                }
            } else {
                data = pasteboard.data(forType: native)
            }
        }
        // Answering with nil rather than staying silent matters: the guest has a
        // client blocked on a pipe, and it closes that pipe when the answer
        // arrives. No answer would leave the paste hanging forever.
        output?(.hostSelectionData(
            token: token, mimeType: mimeType,
            base64: data.map { $0.base64EncodedString() }))
    }
}
