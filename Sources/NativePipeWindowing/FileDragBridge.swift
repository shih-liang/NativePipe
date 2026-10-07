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
    private var outgoingHostURLs: [URL]?
    private var publication: Task<Void, Never>?
    private weak var pendingExportView: NSView?
    private var pendingExportEvent: NSEvent?
    private let reportError: (Error) -> Void
    private var session: NSDraggingSession?
    private var dropped = false
    private var localDrop = false

    init(bridge: WindowBridge, reportError: ((Error) -> Void)? = nil) {
        self.bridge = bridge
        self.reportError = reportError ?? { [weak bridge] error in bridge?.reportFileTransferError(error) }
    }

    func receive(_ message: FileDragMessage) {
        switch message.action {
        case .offered:
            Self.log.debug("Guest offered drag \(message.token)")
            guard session == nil, !dropped, bridge?.fileAccess != nil else { return }
            clearExport()
            outgoing = message.token; outgoingURLs = []
            send(.init(.readSource, token: outgoing))
        case .sourceData:
            guard message.token == outgoing, let data = message.data else { return }
            outgoingURLs = (try? FileTransferURLs.decode(data)) ?? []
            publication?.cancel(); outgoingHostURLs = nil
            if let publish = bridge?.publishGuestFiles, let access = bridge?.fileAccess, !outgoingURLs.isEmpty {
                let token = outgoing, connection = generation, files = outgoingURLs
                publication = Task { @MainActor [weak self] in
                    do {
                        // Preparing names while Linux owns the gesture avoids
                        // waiting for a whole-file transfer at the Mac boundary.
                        let urls = try await publish(files, access, .drag)
                        try Task.checkCancellation()
                        guard let self, self.generation == connection, self.outgoing == token else { return }
                        _ = try FileTransferURLs.decode(FileTransferURLs.encode(urls))
                        guard urls.count == files.count else { throw FileRPC.Failure.protocolError }
                        self.outgoingHostURLs = urls
                        // A delayed mount may finish after mouse-up or after
                        // the source view disappears. Start only the same live
                        // gesture, using metadata-resolved URL representations.
                        if let view = self.pendingExportView, let event = self.pendingExportEvent {
                            self.pendingExportView = nil; self.pendingExportEvent = nil
                            guard NSEvent.pressedMouseButtons & 1 != 0, view.window != nil else {
                                self.clearExport(); return
                            }
                            _ = self.beginExportIfNeeded(view: view, event: event)
                        }
                    } catch {
                        guard !(error is CancellationError), let self,
                              self.generation == connection, self.outgoing == token else { return }
                        self.clearExport()
                        self.reportError(error)
                    }
                }
            } else if !outgoingURLs.isEmpty {
                clearExport(); reportError(GuestFileSharingError.unavailable)
            }
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
        if receivers.isEmpty, !urls.isEmpty { supplyIncomingFiles(); return true }
        let receipt: IncomingDragFiles
        do { receipt = try IncomingDragFiles(info.draggingPasteboard) }
        catch {
            send(.init(.payload, token: token))
            reportError(error)
            return true
        }
        let connection = generation
        transfers[token] = Task { [weak self] in
            defer { withExtendedLifetime(receipt) {} }
            do {
                let imported = try await receipt.importFiles(using: access)
                guard let self, self.generation == connection else { return }
                self.transfers[token] = nil
                self.send(.init(.payload, token: token, data: FileTransferURLs.encode(imported)))
            } catch {
                guard let self, self.generation == connection else { return }
                self.transfers[token] = nil
                self.send(.init(.payload, token: token))
                if !(error is CancellationError) { self.reportError(error) }
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
                if !(error is CancellationError) { self.reportError(error) }
            }
        }
    }

    func beginExportIfNeeded(view: NSView, event: NSEvent) -> Bool {
        guard session == nil, outgoing != 0, !outgoingURLs.isEmpty,
              bridge?.fileAccess != nil,
              bridge?.containsGuestWindow(at: NSEvent.mouseLocation) == false else { return false }
        // The source URI may omit directory metadata. AppKit rejects the
        // generic UTType.item, so never guess .data or block its event loop:
        // wait for the existing publication task to return actual mounted URLs.
        guard let hostURLs = outgoingHostURLs else {
            pendingExportView = view; pendingExportEvent = event
            return false
        }
        pendingExportView = nil; pendingExportEvent = nil
        dropped = false; localDrop = false
        let point = view.convert(event.locationInWindow, from: nil)
        let writers: [any NSPasteboardWriting] = hostURLs.map { $0 as NSURL }
        let items = writers.enumerated().map { i, writer in
            let item = NSDraggingItem(pasteboardWriter: writer)
            let image = NSWorkspace.shared.icon(for: hostURLs[i].hasDirectoryPath ? .folder : .data)
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
        guard dropped else { return }
        Self.log.debug("Drag \(self.outgoing) finished")
        send(.init(.exportEnded, token: outgoing, data: Data([1])))
        clearExport()
    }
    private func clearExport() {
        publication?.cancel(); publication = nil
        pendingExportView = nil; pendingExportEvent = nil
        outgoing = 0; outgoingURLs = []; outgoingHostURLs = nil
        dropped = false; localDrop = false
    }
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
