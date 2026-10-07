import AppKit
import FSKit
import Darwin
import NativePipeProtocol
import NativePipeStrings

/// A machine-session-owned catalog, not a copy of the machine's filesystem.
/// Sharing publishes metadata; the filesystem asks this owner for byte ranges.
@available(macOS 27.0, *)
@MainActor
public final class SharedFileVolumeBroker {
    public let directory: URL
    public let descriptor: SharedFileVolumeDescriptor
    private struct Node {
        var item: SharedFileItem
        let source: URL
        let access: any UserFileRangeAccess
        let selection: UInt64
        var published = false
        var purposes: Set<GuestFileSharePurpose> = []
    }
    private var nodes: [UInt64: Node] = [:]
    private var nextID: UInt64 = 3
    private var pendingShares: [UInt64: [UUID: GuestFileSharePurpose]] = [:]
    private var generation = UUID()
    private var catalogRevision = UUID()
    private var catalogModified = Date()
    private var purposeGenerations: [GuestFileSharePurpose: UUID] = [:]
    private var server: LocalSocketServer?
    private var mountTask: Task<URL, Error>?
    private var mountJob: UUID?
    private var mountedURL: URL?
    private var scopedMount = false
    private var stopped = false
    private var ownsDirectory = false
    private var unmountObserver: NSObjectProtocol?
    private let mount: @MainActor (URL) async throws -> URL
    private let unmount: @MainActor (URL) -> Void
    public var hasPublishedFiles: Bool { nodes.values.contains { $0.item.parentID == 2 && $0.published && !$0.purposes.isEmpty } }
    /// Published files are live service work even when the manager is closed.
    public var publicationDidChange: (() -> Void)?

    public init(directory: URL, name: String,
                filesystemBundleIdentifier: String = "com.nativepipe.cli.filesystem",
                moduleDisplayName: String = "NativePipe Shared Files",
                mount: (@MainActor (URL) async throws -> URL)? = nil,
                unmount: (@MainActor (URL) -> Void)? = nil) {
        self.directory = directory
        var volumeName = name.replacingOccurrences(of: "/", with: "∕").replacingOccurrences(of: "\0", with: "")
        while volumeName.utf8.count > 255 { volumeName.removeLast() }
        descriptor = .init(id: UUID(), name: volumeName.isEmpty ? moduleDisplayName : volumeName, token: UUID())
        self.mount = mount ?? { directory in
            let modules = try await FSClient.shared.installedExtensions
            guard modules.contains(where: { $0.bundleIdentifier == filesystemBundleIdentifier && $0.isEnabled }) else {
                throw NSError(domain: "NativePipe.FileSharing", code: Int(ENODEV), userInfo: [NSLocalizedDescriptionKey:
                    NPText("Enable %@ in System Settings → General → Login Items & Extensions → File System Extensions before sharing files.", moduleDisplayName)])
            }
            let bookmark = try directory.bookmarkData(options: [.withSecurityScope, .securityScopeAllowOnlyReadAccess])
            var stale = false
            let resource = try URL(resolvingBookmarkData: bookmark, options: [.withSecurityScope, .withoutUI],
                                   relativeTo: nil, bookmarkDataIsStale: &stale)
            guard !stale, resource.startAccessingSecurityScopedResource() else { throw POSIXError(.EACCES) }
            defer { resource.stopAccessingSecurityScopedResource() }
            return try await FSClient.shared.mountSingleVolume(
                resource: FSPathURLResource(url: resource, writable: false),
                bundleID: filesystemBundleIdentifier, options: ["rdonly"])
        }
        self.unmount = unmount ?? { url in
            // Final process teardown must finish this native request before
            // exit; an un-awaited task can leave a dead volume in Finder.
            do { try NSWorkspace.shared.unmountAndEjectDevice(at: url) }
            catch { NSLog("%@", NPText("The shared volume could not be ejected: %@", error.localizedDescription)) }
        }
        unmountObserver = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didUnmountNotification, object: nil, queue: .main) { [weak self] notification in
                guard let url = notification.userInfo?[NSWorkspace.volumeURLUserInfoKey] as? URL else { return }
                MainActor.assumeIsolated { self?.didUnmount(url) }
            }
    }

    deinit {
        if let unmountObserver { NSWorkspace.shared.notificationCenter.removeObserver(unmountObserver) }
    }

    public func publish(_ files: [URL], using access: any UserFileAccess,
                        purpose: GuestFileSharePurpose = .files) async throws -> [URL] {
        guard !stopped, let rangeAccess = access as? any UserFileRangeAccess else { throw POSIXError(.ENOTSUP) }
        _ = try FileTransferURLs.decode(FileTransferURLs.encode(files))
        let token = generation
        let purposeToken = purposeGenerations[purpose]
        var metadata: [UserFileMetadata] = []
        for file in files {
            try UserFileRange.validate(file)
            let info = try await rangeAccess.metadata(for: file)
            guard generation == token, purposeGenerations[purpose] == purposeToken, !stopped else { throw CancellationError() }
            try Self.validate(info, source: file)
            metadata.append(info)
        }
        try Task.checkCancellation()
        try start()
        var ids: [UInt64] = []
        for info in metadata {
            if let existing = nodes.values.first(where: {
                $0.item.parentID == 2 && $0.access === rangeAccess && Self.sameSource($0.source, info.url)
            }) {
                if existing.item.kind == .directory, existing.item.version != info.version {
                    removeChildren(existing.item.id)
                }
                nodes[existing.item.id]?.item = item(info, id: existing.item.id, parent: 2, name: existing.item.name)
                ids.append(existing.item.id)
            } else {
                let id = allocateID()
                let name = uniqueName(info.url.lastPathComponent)
                nodes[id] = Node(item: item(info, id: id, parent: 2, name: name),
                                 source: info.url, access: rangeAccess, selection: id)
                ids.append(id)
            }
        }
        let publication = UUID()
        ids.forEach { pendingShares[$0, default: [:]][publication] = purpose }
        catalogChanged()
        defer {
            for id in ids {
                pendingShares[id]?.removeValue(forKey: publication)
                if pendingShares[id]?.isEmpty == true { pendingShares[id] = nil }
                if pendingShares[id] == nil, nodes[id]?.purposes.isEmpty == true { removeTree(id) }
            }
            catalogChanged()
        }
        let root = try await mountVolume()
        try Task.checkCancellation()
        guard generation == token, purposeGenerations[purpose] == purposeToken, !stopped else { throw CancellationError() }
        let urls = try ids.map { id in
            guard let item = nodes[id]?.item else { throw POSIXError(.ESTALE) }
            return root.appendingPathComponent(item.name, isDirectory: item.kind == .directory)
        }
        let wasPublished = hasPublishedFiles
        ids.forEach { nodes[$0]?.published = true; nodes[$0]?.purposes.insert(purpose) }
        if hasPublishedFiles != wasPublished { publicationDidChange?() }
        return urls
    }

    /// Disconnect and policy changes revoke the old inode capabilities. IDs
    /// never repeat, including after reconnecting this machine owner.
    public func revoke(purpose: GuestFileSharePurpose? = nil) {
        let wasPublished = hasPublishedFiles
        defer {
            catalogChanged()
            if hasPublishedFiles != wasPublished { publicationDidChange?() }
        }
        if let purpose {
            purposeGenerations[purpose] = UUID()
            let roots = nodes.values.filter {
                $0.item.parentID == 2 && ($0.purposes.contains(purpose) || pendingShares[$0.item.id]?.values.contains(purpose) == true)
            }
            for node in roots {
                pendingShares[node.item.id] = pendingShares[node.item.id]?.filter { $0.value != purpose }
                if pendingShares[node.item.id]?.isEmpty == true { pendingShares[node.item.id] = nil }
                nodes[node.item.id]?.purposes.remove(purpose)
                if nodes[node.item.id]?.purposes.isEmpty == true, pendingShares[node.item.id] == nil { removeTree(node.item.id) }
            }
            let remaining = Set(nodes.values.map { ObjectIdentifier($0.access) })
            var closed = Set<ObjectIdentifier>()
            for node in roots where !remaining.contains(ObjectIdentifier(node.access)) {
                if closed.insert(ObjectIdentifier(node.access)).inserted { node.access.closeRangeAccess() }
            }
            return
        }
        generation = UUID()
        let access = Dictionary(nodes.values.map { (ObjectIdentifier($0.access), $0.access) }, uniquingKeysWith: { first, _ in first }).values
        nodes.removeAll()
        pendingShares.removeAll()
        access.forEach { $0.closeRangeAccess() }
        mountTask?.cancel(); mountTask = nil; mountJob = nil
    }
    public func stop() {
        guard !stopped else { return }
        stopped = true
        revoke()
        if let unmountObserver { NSWorkspace.shared.notificationCenter.removeObserver(unmountObserver); self.unmountObserver = nil }
        server?.stop(); server = nil
        if let mountedURL {
            if scopedMount { mountedURL.stopAccessingSecurityScopedResource(); scopedMount = false }
            unmount(mountedURL)
        }
        self.mountedURL = nil
        if ownsDirectory { try? FileManager.default.removeItem(at: directory); ownsDirectory = false }
    }

    private func didUnmount(_ url: URL) {
        guard let mountedURL, Data(url.standardizedFileURL.path.utf8) == Data(mountedURL.standardizedFileURL.path.utf8) else { return }
        if scopedMount { mountedURL.stopAccessingSecurityScopedResource(); scopedMount = false }
        self.mountedURL = nil
        // Finder ejects the catalog, including outstanding reads. A subsequent
        // explicit share gets a fresh mount and fresh inode capabilities.
        revoke()
    }

    private func start() throws {
        guard server == nil else { return }
        // sockadddr_un paths are bounded independently of source file paths.
        guard directory.appendingPathComponent("files.sock").path.utf8.count < 104 else { throw POSIXError(.ENAMETOOLONG) }
        try FileManager.default.createDirectory(at: directory.deletingLastPathComponent(), withIntermediateDirectories: true)
        guard mkdir(directory.path, 0o700) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        ownsDirectory = true
        do {
            let url = directory.appendingPathComponent("volume.json")
            try JSONEncoder().encode(descriptor).write(to: url, options: .atomic)
            guard chmod(url.path, 0o600) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
            let server = LocalSocketServer(url: directory.appendingPathComponent("files.sock"), maximumConnections: 8)
            try server.start { [weak self] connection in await self?.serve(connection) }
            self.server = server
        } catch { try? FileManager.default.removeItem(at: directory); ownsDirectory = false; throw error }
    }
    private func mountVolume() async throws -> URL {
        if let mountedURL { return mountedURL }
        if let mountTask { return try await mountTask.value }
        let token = generation, job = UUID(), mount = mount, unmount = unmount, directory = directory
        mountJob = job
        let task = Task { @MainActor [weak self] in
            let url = try await mount(directory)
            guard let self, !stopped, generation == token else {
                unmount(url)
                throw CancellationError()
            }
            mountedURL = url
            scopedMount = url.startAccessingSecurityScopedResource()
            return url
        }
        mountTask = task
        do {
            let url = try await task.value
            if mountJob == job { mountTask = nil; mountJob = nil }
            return url
        } catch {
            if mountJob == job { mountTask = nil; mountJob = nil }
            throw error
        }
    }
    private func allocateID() -> UInt64 { let id = nextID; nextID += 1; return id }
    private static func validName(_ name: String) -> Bool {
        !name.isEmpty && name != "." && name != ".." && !name.contains("/") && !name.contains("\0") && name.utf8.count <= 255
    }
    private static func validate(_ info: UserFileMetadata, source: URL) throws {
        try UserFileRange.validate(info.url)
        guard sameSource(info.url, source),
              info.kind == .file || info.kind == .directory,
              validName(info.url.lastPathComponent), !info.version.isEmpty,
              info.version.count <= UserFileRange.maximumVersionLength,
              info.size <= UInt64(Int64.max) else { throw POSIXError(.EINVAL) }
        if let seconds = info.modified?.timeIntervalSince1970 {
            guard seconds.isFinite, seconds > Double(Int.min), seconds < Double(Int.max) else { throw POSIXError(.EINVAL) }
        }
    }
    private static func sameSource(_ first: URL, _ second: URL) -> Bool {
        Data(first.path.utf8) == Data(second.path.utf8)
    }
    private func uniqueName(_ sourceName: String, parent: UInt64 = 2) -> String {
        // Native file URLs normalize their names. Keep the remote source bytes
        // separately, and disambiguate canonically equivalent Mac names.
        var original = sourceName.decomposedStringWithCanonicalMapping
        while original.utf8.count > 255 { original.removeLast() }
        if original.isEmpty { original = NPText("Shared File") }
        let used = Set(nodes.values.filter { $0.item.parentID == parent }.map { Data($0.item.name.utf8) })
        if !used.contains(Data(original.utf8)) { return original }
        let path = original as NSString, ext = path.pathExtension
        let suffixExtension = ext.utf8.count < 240 ? ext : ""
        let stem = suffixExtension.isEmpty ? original : path.deletingPathExtension
        var count = 2
        while true {
            let suffix = " (\(count))" + (suffixExtension.isEmpty ? "" : "." + suffixExtension)
            // Truncation preserves Unicode boundaries and the filename limit.
            var base = stem
            while (base + suffix).utf8.count > 255 { base.removeLast() }
            let name = base + suffix
            if !used.contains(Data(name.utf8)) { return name }; count += 1
        }
    }
    private func item(_ info: UserFileMetadata, id: UInt64, parent: UInt64, name: String) -> SharedFileItem {
        .init(id: id, parentID: parent, name: name, kind: info.kind == .directory ? .directory : .file,
              size: info.size, modified: info.modified, permissions: info.permissions, version: info.version)
    }
    private func catalogChanged() { catalogRevision = UUID(); catalogModified = Date() }
    private func isVisible(_ node: Node) -> Bool {
        (node.published && !node.purposes.isEmpty) || pendingShares[node.selection]?.isEmpty == false
    }
    private func root() -> SharedFileItem {
        .init(id: 2, parentID: 2, name: descriptor.name, kind: .directory, size: 0,
              modified: catalogModified, permissions: 0o500, version: Data(catalogRevision.uuidString.utf8))
    }
    private func removeTree(_ id: UInt64) {
        removeChildren(id)
        nodes.removeValue(forKey: id)
    }
    private func removeChildren(_ id: UInt64) {
        for child in nodes.values.filter({ $0.item.parentID == id }).map({ $0.item.id }) { removeTree(child) }
    }
    private func refreshed(_ id: UInt64) async throws -> SharedFileItem {
        if id == 2 { return root() }
        guard let node = nodes[id] else { throw POSIXError(.ESTALE) }
        let token = generation
        let info = try await node.access.metadata(for: node.source)
        guard generation == token, nodes[id] != nil, !stopped else { throw POSIXError(.ESTALE) }
        try Self.validate(info, source: node.source)
        if node.item.kind == .directory, node.item.version != info.version || info.kind != .directory {
            removeChildren(id)
        }
        let value = item(info, id: id, parent: node.item.parentID, name: node.item.name)
        nodes[id]?.item = value
        return value
    }
    private func children(_ id: UInt64) async throws -> [SharedFileItem] {
        // Native mount/probe may enumerate before mountSingleVolume returns.
        // Pending selections already have user consent and must be visible then.
        if id == 2 { return nodes.values.filter { $0.item.parentID == 2 && isVisible($0) }.map(\.item).sorted { $0.name < $1.name } }
        guard let node = nodes[id], node.item.kind == .directory else { throw POSIXError(.ENOTDIR) }
        let token = generation
        let children = try await node.access.contents(of: node.source)
        guard generation == token, nodes[id] != nil, !stopped else { throw POSIXError(.ESTALE) }
        var names = Set<Data>()
        for info in children where info.kind == .file || info.kind == .directory {
            try Self.validate(info, source: info.url)
            guard Self.sameSource(info.url.deletingLastPathComponent(), node.source),
                  names.insert(Data(info.url.lastPathComponent.utf8)).inserted else { throw POSIXError(.EACCES) }
        }
        var result: [SharedFileItem] = []
        for info in children where info.kind == .file || info.kind == .directory {
            let existing = nodes.values.first { $0.item.parentID == id && Self.sameSource($0.source, info.url) }
            let childID = existing?.item.id ?? allocateID()
            if let existing, existing.item.kind == .directory,
               existing.item.version != info.version || info.kind != .directory { removeChildren(childID) }
            let value = item(info, id: childID, parent: id,
                             name: existing?.item.name ?? uniqueName(info.url.lastPathComponent, parent: id))
            nodes[childID] = Node(item: value, source: info.url, access: node.access, selection: node.selection)
            result.append(value)
        }
        let present = Set(result.map(\.id))
        for missing in nodes.values.filter({ $0.item.parentID == id && !present.contains($0.item.id) }).map({ $0.item.id }) { removeTree(missing) }
        return result.sorted { $0.name < $1.name }
    }
    private func read(_ id: UInt64, offset: UInt64, length: Int, version: Data) async throws -> Data {
        guard let node = nodes[id], node.item.kind == .file else { throw POSIXError(.ESTALE) }
        guard let selection = nodes[node.selection], isVisible(selection) else { throw POSIXError(.EACCES) }
        guard version == node.item.version else { throw POSIXError(.ESTALE) }
        try UserFileRange.validate(node.source, offset: offset, length: length)
        let token = generation
        let bytes = try await node.access.read(node.source, offset: offset, length: length, expectedVersion: version)
        guard generation == token, nodes[id]?.item.version == version,
              nodes[id].map({ Self.sameSource($0.source, node.source) }) == true, nodes[id]?.access === node.access,
              bytes.count <= length, !stopped else { throw POSIXError(.ESTALE) }
        return bytes
    }
    private func serve(_ connection: SocketConnection) async {
        do {
            let request = try JSONDecoder().decode(SharedFileRequest.self,
                from: await FileRPC.receiveRecord(from: connection, maximum: 16 * 1024, deadline: .now() + .seconds(5)))
            guard request.token == descriptor.token, !stopped else { throw POSIXError(.EACCES) }
            try await withThrowingTaskGroup(of: Void.self) { group in
                group.addTask { @MainActor in
                    do { try await self.reply(request.operation, to: connection) }
                    catch { try await self.send(.init(error: Self.errorCode(error)), to: connection) }
                }
                group.addTask {
                    _ = try await connection.readExactly(1, deadline: .distantFuture)
                    throw CancellationError()
                }
                defer { group.cancelAll() }
                _ = try await group.next()
            }
        } catch {
            try? await send(.init(error: Self.errorCode(error)), to: connection)
        }
    }
    private func reply(_ operation: SharedFileOperation, to connection: SocketConnection) async throws {
        switch operation {
        case .item(let id): try await send(.init(item: refreshed(id)), to: connection)
        case .children(let id):
            let values = try await children(id)
            if values.isEmpty { try await send(.init(items: []), to: connection) }
            for start in stride(from: 0, to: values.count, by: 128) {
                try await send(.init(items: Array(values[start..<min(start + 128, values.count)]), more: start + 128 < values.count), to: connection)
            }
        case .read(let id, let offset, let length, let version):
            guard length >= 0, length <= UserFileRange.maximumReadLength, !version.isEmpty,
                  version.count <= UserFileRange.maximumVersionLength else { throw POSIXError(.EINVAL) }
            try await send(.init(data: read(id, offset: offset, length: length, version: version)), to: connection)
        }
    }
    private func send(_ reply: SharedFileReply, to connection: SocketConnection) async throws {
        try await FileRPC.sendRecord(try JSONEncoder().encode(reply), to: connection, deadline: .now() + .seconds(30))
    }
    static func errorCode(_ error: Error) -> Int32 {
        if let error = error as? POSIXError { return error.code.rawValue }
        if error is CancellationError { return ECANCELED }
        if let error = error as? FileRPC.Failure {
            switch error {
            case .local(let code): return code
            case .remote(let code):
                return switch code {
                case 1: EPERM
                case 2: ENOENT
                case 13: EACCES
                case 20: ENOTDIR
                case 21: EISDIR
                case 22: EINVAL
                case 38: ENOSYS
                case 40: ELOOP
                case 95: ENOTSUP
                case 116: ESTALE
                case 125: ECANCELED
                default: EIO
                }
            case .sourceChanged: return ESTALE
            case .invalidPath: return EINVAL
            default: return EIO
            }
        }
        return EIO
    }
}
