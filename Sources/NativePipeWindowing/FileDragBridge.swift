import AppKit
import NativePipeProtocol
import OSLog

/// One AppKit/Wayland file-drag state machine for VMHost and RemoteHost.
/// Intra-Linux drags keep their original Wayland path until the pointer exits
/// our windows; only then does AppKit take over the external drag.
@MainActor
final class FileDragBridge: NSObject, NSDraggingSource {
    private static let log = Logger(subsystem: "com.nativepipe", category: "FileDrag")
    weak var bridge: WindowBridge?
    private var nextToken: UInt32 = 0
    private var incoming: UInt32 = 0
    private var accepted = false
    private var target: UInt32 = 0
    private var incomingSequence = -1
    private var incomingURLs: [URL] = []
    private var importedURLs: [URL]?
    private var transfers: [UInt32: Task<Void, Never>] = [:]
    private var droppedImports: Set<UInt32> = []
    private var generation: UInt64 = 0
    private var outgoing: UInt32 = 0
    private var outgoingURLs: [URL] = []
    private var promises: [LinuxFilePromise] = []
    private var session: NSDraggingSession?
    private var dropped = false
    private var failed = false
    private var remaining = 0
    private var localDrop = false

    init(bridge: WindowBridge) { self.bridge = bridge }

    func receive(_ message: FileDragMessage) {
        switch message.action {
        case .offered:
            Self.log.debug("Guest offered drag \(message.token)")
            guard session == nil, !dropped, bridge?.fileAccess != nil else { return }
            outgoing = message.token; outgoingURLs = []
            send(.init(.readSource, token: outgoing))
        case .sourceData:
            guard message.token == outgoing, let data = message.data else { return }
            outgoingURLs = (try? FileTransferURLs.decode(data)) ?? []
            Self.log.debug("Drag \(message.token) offers \(self.outgoingURLs.count) files")
        case .accepted:
            if message.token == incoming { accepted = message.data == Data([1]) }
        case .finished:
            transfers[message.token]?.cancel()
            transfers[message.token] = nil
            droppedImports.remove(message.token)
            if message.token == incoming, localDrop {
                send(.init(.exportEnded, token: outgoing, data: message.data ?? Data([0])))
                clearExport()
            }
        case .requestData:
            if message.token == incoming { supplyIncomingFiles() }
        default: break
        }
    }

    func destination(_ info: NSDraggingInfo, window: UInt32, point: CGPoint, entering: Bool) -> NSDragOperation {
        guard bridge?.fileAccess != nil, info.draggingSourceOperationMask.contains(.copy) else { return [] }
        let own = (info.draggingSource as AnyObject?) === self
        guard own || info.draggingPasteboard.canReadObject(forClasses: [NSURL.self, NSFilePromiseReceiver.self],
            options: [.urlReadingFileURLsOnly: true]) else { return [] }
        if incomingSequence != info.draggingSequenceNumber {
            if !droppedImports.contains(incoming) {
                transfers[incoming]?.cancel(); transfers[incoming] = nil
            }
            repeat { nextToken &+= 1 } while nextToken == 0
            incoming = nextToken; incomingSequence = info.draggingSequenceNumber
            importedURLs = nil
            incomingURLs = own ? outgoingURLs : (info.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? [])
            if own { importedURLs = outgoingURLs }
        }
        if entering || target != window {
            target = window; accepted = false
            send(.init(.enter, token: incoming, window: window, x: point.x, y: point.y))
            return .copy // Negotiation completes asynchronously at the next update.
        }
        send(.init(.motion, token: incoming, window: window, x: point.x, y: point.y))
        return accepted ? .copy : []
    }

    func leave() {
        if incoming != 0 { send(.init(.leave, token: incoming)) }
        target = 0; accepted = false
    }

    func perform(_ info: NSDraggingInfo) -> Bool {
        guard accepted, let access = bridge?.fileAccess else { return false }
        let token = incoming
        let own = (info.draggingSource as AnyObject?) === self
        let urls = info.draggingPasteboard.readObjects(forClasses: [NSURL.self],
            options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
        let receivers = info.draggingPasteboard.readObjects(forClasses: [NSFilePromiseReceiver.self]) as? [NSFilePromiseReceiver] ?? []
        guard own || !urls.isEmpty || !receivers.isEmpty else { return false }
        send(.init(.drop, token: token))
        droppedImports.insert(token)
        if own {
            localDrop = true
            send(.init(.exportDropped, token: outgoing))
            send(.init(.payload, token: token, data: FileTransferURLs.encode(outgoingURLs)))
            return true
        }
        if !urls.isEmpty { supplyIncomingFiles(); return true }
        let receipt: IncomingFilePromises
        do { receipt = try IncomingFilePromises(receivers) }
        catch {
            send(.init(.payload, token: token))
            bridge?.reportFileTransferError(error)
            return true
        }
        let connection = generation
        transfers[token] = Task { [weak self] in
            defer { withExtendedLifetime(receipt) {} }
            do {
                let selected = try await receipt.files()
                // Promised directories are temporary: export their contents,
                // never leave a virtiofs share pointing into deleted staging.
                let imported = try await access.importFiles(selected, shareDirectories: false)
                guard let self, self.generation == connection else { return }
                self.transfers[token] = nil
                self.send(.init(.payload, token: token, data: FileTransferURLs.encode(imported)))
            } catch {
                guard let self, self.generation == connection else { return }
                self.transfers[token] = nil
                self.send(.init(.payload, token: token))
                if !(error is CancellationError) { self.bridge?.reportFileTransferError(error) }
            }
        }
        return true
    }

    private func supplyIncomingFiles() {
        let token = incoming
        if let importedURLs {
            send(.init(.payload, token: token, data: FileTransferURLs.encode(importedURLs)))
            return
        }
        guard transfers[token] == nil, !incomingURLs.isEmpty, let access = bridge?.fileAccess else { return }
        let urls = incomingURLs
        let connection = generation
        transfers[token] = Task { [weak self] in
            do {
                let imported = try await access.importFiles(urls)
                guard let self, self.generation == connection else { return }
                self.transfers[token] = nil
                if self.incoming == token { self.importedURLs = imported }
                self.send(.init(.payload, token: token, data: FileTransferURLs.encode(imported)))
            } catch {
                guard let self, self.generation == connection else { return }
                self.transfers[token] = nil
                self.send(.init(.payload, token: token))
                if !(error is CancellationError) { self.bridge?.reportFileTransferError(error) }
            }
        }
    }

    func beginExportIfNeeded(view: NSView, event: NSEvent) -> Bool {
        guard session == nil, outgoing != 0, !outgoingURLs.isEmpty,
              let access = bridge?.fileAccess,
              bridge?.containsGuestWindow(at: NSEvent.mouseLocation) == false else { return false }
        promises = outgoingURLs.map { LinuxFilePromise(remote: $0, access: access) }
        remaining = promises.count; dropped = false; failed = false; localDrop = false
        let token = outgoing
        for promise in promises {
            promise.completed = { [weak self] error in
                guard let self, self.outgoing == token else { return }
                self.failed = self.failed || error != nil
                self.remaining -= 1
                self.finishExportIfReady()
            }
        }
        let point = view.convert(event.locationInWindow, from: nil)
        let items = promises.enumerated().map { i, promise in
            let item = NSDraggingItem(pasteboardWriter: promise.provider)
            let image = NSWorkspace.shared.icon(for: .data)
            item.setDraggingFrame(NSRect(x: point.x + CGFloat(i * 4), y: point.y,
                width: 48, height: 48), contents: image)
            return item
        }
        send(.init(.exportBegan, token: outgoing))
        Self.log.debug("AppKit begins drag \(self.outgoing)")
        bridge?.hideDragIcon()
        session = view.beginDraggingSession(with: items, event: event, source: self)
        return true
    }

    func draggingSession(_ session: NSDraggingSession, sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation { .copy }

    func draggingSession(_ session: NSDraggingSession, endedAt screenPoint: NSPoint, operation: NSDragOperation) {
        Self.log.debug("AppKit ended drag \(self.outgoing), operation \(operation.rawValue)")
        self.session = nil
        guard outgoing != 0 else { return }
        if operation.contains(.copy) {
            dropped = true
            send(.init(.exportDropped, token: outgoing))
            if !localDrop { finishExportIfReady() }
        } else {
            send(.init(.exportEnded, token: outgoing, data: Data([0])))
            clearExport()
        }
    }
    private func finishExportIfReady() {
        guard dropped, remaining == 0 else { return }
        Self.log.debug("Drag \(self.outgoing) transfers finished, failed \(self.failed)")
        send(.init(.exportEnded, token: outgoing, data: Data([failed ? 0 : 1])))
        clearExport()
    }
    private func clearExport() { outgoing = 0; outgoingURLs = []; promises = []; dropped = false; localDrop = false }
    func pointerReleased() { if session == nil && !dropped { clearExport() } }
    private func send(_ message: FileDragMessage) { bridge?.send(.fileDrag(message)) }
    func disconnect() {
        generation &+= 1
        for task in transfers.values { task.cancel() }
        transfers.removeAll(); droppedImports.removeAll()
        incoming = 0; incomingSequence = -1; target = 0; accepted = false
        importedURLs = nil; incomingURLs = []
        clearExport()
    }
}
