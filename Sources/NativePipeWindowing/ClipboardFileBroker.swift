import AppKit
import Darwin
import NativePipeProtocol

/// An on-demand file selection between helpers sharing an IPC directory.
/// Copy publishes only an opaque capability. Paste exports through UserFileAccess,
/// then atomically hands staging to the receiver before it imports into its guest.
/// AppKit file promises remain a drag adapter; they cannot receive ordinary paste.
@MainActor
final class ClipboardFileBroker {
    static let pasteboardType = NSPasteboard.PasteboardType("com.nativepipe.clipboard-files.v1")
    static var defaultDirectory: URL {
        URL(fileURLWithPath: "/tmp/nativepipe-clipboard-\(getuid())", isDirectory: true)
    }

    struct Offer: Codable {
        let socket: String
        let token: UUID
        let names: [String]
    }
    private enum Reply: Codable { case ready(String), failed(String) }
    enum Failure: LocalizedError {
        case unavailable, transfer(String)
        var errorDescription: String? {
            switch self {
            case .unavailable: return "The source file selection is no longer available. Copy the files again."
            case .transfer(let message): return message
            }
        }
    }
    private struct Selection {
        let offer: Offer
        let files: [URL]
        let access: any UserFileAccess
    }
    private let directory: URL
    private let socket: URL
    private let server: LocalSocketServer
    private var selection: Selection?

    init(directory: URL) {
        self.directory = directory.standardizedFileURL.resolvingSymlinksInPath()
        socket = self.directory.appendingPathComponent("c-" + UUID().uuidString.prefix(16) + ".sock")
        server = LocalSocketServer(url: socket, maximumConnections: 4)
    }
    deinit { try? FileManager.default.removeItem(at: socket.appendingPathExtension("lock")) }

    func publish(_ files: [URL], using access: any UserFileAccess, to pasteboard: NSPasteboard) throws {
        _ = try FileTransferURLs.decode(FileTransferURLs.encode(files))
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        try server.start { [weak self] connection in
            do { try await self?.serve(connection) }
            catch { /* EOF reports failure to the requesting clipboard bridge. */ }
        }
        let offer = Offer(socket: socket.lastPathComponent, token: UUID(), names: files.map(\.lastPathComponent))
        let data = try JSONEncoder().encode(offer)
        selection = Selection(offer: offer, files: files, access: access)
        pasteboard.declareTypes([Self.pasteboardType], owner: nil)
        guard pasteboard.setData(data, forType: Self.pasteboardType) else {
            selection = nil
            throw FileRPC.Failure.protocolError
        }
    }

    /// Replacing the clipboard revokes new requests, but an accepted paste keeps
    /// its captured selection. Disconnect/policy changes additionally cancel IO.
    func revoke() { selection = nil }
    func stop() {
        selection = nil
        server.stop()
        try? FileManager.default.removeItem(at: socket.appendingPathExtension("lock"))
    }

    static func offer(from pasteboard: NSPasteboard) -> Offer? {
        guard let data = pasteboard.data(forType: pasteboardType), data.count <= 1024 * 1024,
              let offer = try? JSONDecoder().decode(Offer.self, from: data),
              offer.socket.hasPrefix("c-"), offer.socket.hasSuffix(".sock"),
              validName(offer.socket), !offer.names.isEmpty, offer.names.count <= 1024,
              offer.names.allSatisfy(validName) else { return nil }
        return offer
    }

    private static func validName(_ name: String) -> Bool {
        !name.isEmpty && name != "." && name != ".." && !name.contains("/") &&
            !name.contains("\0") && name.utf8.count <= 255
    }

    private func serve(_ connection: SocketConnection) async throws {
        let request = try await FileRPC.receiveRecord(from: connection, maximum: 128, deadline: .now() + .seconds(5))
        guard let token = try? JSONDecoder().decode(UUID.self, from: request),
              let selection, selection.offer.token == token else { throw FileRPC.Failure.invalidPath }
        let staging = try Receipt.create(in: directory, prefix: "p-")
        // Monitor EOF concurrently with export: cancelling a paste must also
        // cancel its SFTP/vsock operation, not just abandon its eventual reply.
        try await withThrowingTaskGroup(of: Void.self) { group in
            group.addTask { @MainActor in
                do {
                    for (index, file) in selection.files.enumerated() {
                        try Task.checkCancellation()
                        let parent = staging.directory.appendingPathComponent(String(index))
                        try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: false)
                        try await selection.access.exportFile(file, to: parent.appendingPathComponent(file.lastPathComponent))
                    }
                    try Task.checkCancellation()
                    try await FileRPC.sendRecord(try JSONEncoder().encode(Reply.ready(staging.directory.lastPathComponent)),
                        to: connection, deadline: .now() + .seconds(5))
                } catch {
                    try? await FileRPC.sendRecord(try JSONEncoder().encode(Reply.failed(String(error.localizedDescription.prefix(8192)))),
                        to: connection, deadline: .now() + .seconds(5))
                    throw error
                }
            }
            group.addTask {
                // Bounded stalled clients, including those that never claim the
                // reply. Source transfers already have their own IO deadlines.
                let ack = try await connection.readExactly(1, deadline: .now() + .seconds(600))
                guard ack == Data([1]) else { throw FileRPC.Failure.protocolError }
            }
            defer { group.cancelAll() }
            while try await group.next() != nil {}
        }
        withExtendedLifetime(staging) {}
    }

    func receive(_ offer: Offer) async throws -> Receipt {
        guard Self.validName(offer.socket), offer.socket.hasPrefix("c-"), offer.socket.hasSuffix(".sock"),
              !offer.names.isEmpty, offer.names.count <= 1024, offer.names.allSatisfy(Self.validName)
        else { throw FileRPC.Failure.invalidPath }
        let connection = try await SocketConnection.connect(to: directory.appendingPathComponent(offer.socket))
        defer { connection.close() }
        try await FileRPC.sendRecord(try JSONEncoder().encode(offer.token), to: connection, deadline: .now() + .seconds(5))
        let data: Data
        do { data = try await FileRPC.receiveRecord(from: connection, maximum: 64 * 1024, deadline: .now() + .seconds(600)) }
        catch is SocketConnection.Failure { throw Failure.unavailable }
        let name: String
        switch try JSONDecoder().decode(Reply.self, from: data) {
        case .ready(let value): name = value
        case .failed(let message): throw Failure.transfer(message)
        }
        guard name.hasPrefix("p-"),
              UUID(uuidString: String(name.dropFirst(2))) != nil else { throw FileRPC.Failure.invalidPath }
        try Task.checkCancellation()
        let receipt = try Receipt.claim(directory.appendingPathComponent(name), in: directory, names: offer.names)
        // Claim renames staging within the shared directory. The source may now
        // die or disconnect without deleting files from under the importer.
        try await connection.write(Data([1]), deadline: .now() + .seconds(5))
        return receipt
    }

    final class Receipt {
        let directory: URL
        private(set) var urls: [URL] = []
        private init(directory: URL) { self.directory = directory }

        static func create(in parent: URL, prefix: String) throws -> Receipt {
            let directory = parent.appendingPathComponent(prefix + UUID().uuidString, isDirectory: true)
            guard mkdir(directory.path, 0o700) == 0 else { throw FileRPC.Failure.local(errno) }
            return Receipt(directory: directory)
        }
        static func claim(_ source: URL, in parent: URL, names: [String]) throws -> Receipt {
            var info = stat()
            guard lstat(source.path, &info) == 0, (info.st_mode & S_IFMT) == S_IFDIR,
                  info.st_uid == getuid() else { throw FileRPC.Failure.invalidPath }
            let target = parent.appendingPathComponent("r-" + UUID().uuidString, isDirectory: true)
            guard renameatx_np(AT_FDCWD, source.path, AT_FDCWD, target.path, UInt32(RENAME_EXCL)) == 0
            else { throw FileRPC.Failure.local(errno) }
            let receipt = Receipt(directory: target)
            receipt.urls = try names.enumerated().map { index, name in
                let url = target.appendingPathComponent(String(index)).appendingPathComponent(name)
                guard url.resolvingSymlinksInPath().standardizedFileURL == url.standardizedFileURL,
                      lstat(url.path, &info) == 0,
                      (info.st_mode & S_IFMT) == S_IFREG || (info.st_mode & S_IFMT) == S_IFDIR
                else { throw FileRPC.Failure.invalidPath }
                return url
            }
            return receipt
        }
        deinit { try? FileManager.default.removeItem(at: directory) }
    }
}
