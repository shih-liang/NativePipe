import Darwin
import Foundation
import FSKit
import NativePipeFileSharing

@available(macOS 27.0, *)
final class SharedFilesItem: FSItem {
    var metadata: SharedFileItem
    init(_ metadata: SharedFileItem) { self.metadata = metadata; super.init() }
}

/// A read-only view of explicitly shared items. Native FSKit owns the mount and
/// invokes range reads; the broker checks selection, session and version on I/O.
@available(macOS 27.0, *)
final class SharedFilesVolume: FSVolume, FSVolume.Handler, FSVolume.ReadWriteHandler,
                               FSVolume.DataCacheHandler, FSVolume.XattrHandler,
                               FSVolume.AccessCheckHandler, @unchecked Sendable {
    let client: SharedFileVolumeClient
    let state = DispatchQueue(label: "com.nativepipe.filesystem.items")
    let cacheUpdates = DispatchQueue(label: "com.nativepipe.filesystem.cache")
    private var items: [UInt64: SharedFilesItem] = [:]
    // A replaced object may still represent a vnode in the kernel. Hold it
    // until FSKit's synchronized tryReclaim confirms that vnode has retired.
    private var retiredItems: [ObjectIdentifier: SharedFilesItem] = [:]
    private var active = true
    private struct Pending {
        let task: Task<Void, Never>
        let replyCancelled: () -> Void
    }
    private var pending: [UUID: Pending] = [:]
    private let quarantine: Data
    private let fileSystemTypeName: String

    init(client: SharedFileVolumeClient, fileSystemTypeName: String = "nativepipe") {
        self.client = client
        self.fileSystemTypeName = fileSystemTypeName
        quarantine = Data("0081;\(String(Int(Date().timeIntervalSince1970), radix: 16));NativePipe;\(client.descriptor.id.uuidString)".utf8)
        super.init(volumeID: FSVolume.Identifier(uuid: client.descriptor.id),
                   volumeName: FSFileName(string: client.descriptor.name))
    }

    var maximumLinkCount: Int { 1 }
    var maximumNameLength: Int { 255 }
    var maximumFileSize: UInt64 { UInt64(Int64.max) }
    var maximumXattrSize: Int { 1024 }
    var restrictsOwnershipChanges: Bool { true }
    var truncatesLongNames: Bool { false }
    var requestedMountOptions: FSVolume.MountOptions { .readOnly }
    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let capabilities = FSVolume.SupportedCapabilities()
        capabilities.supports64BitObjectIDs = true
        capabilities.caseFormat = .sensitive
        capabilities.supports2TBFiles = true
        capabilities.supportsFastStatFS = true
        capabilities.doesNotSupportVolumeSizes = true
        capabilities.doesNotSupportSettingFilePermissions = true
        return capabilities
    }
    var volumeStatistics: FSStatFSResult {
        let statistics = FSStatFSResult(fileSystemTypeName: fileSystemTypeName)
        statistics.blockSize = 4096
        statistics.ioSize = 256 * 1024
        return statistics
    }

    /// Results and attributes are populated together on the same queue. Lookup
    /// replies and tryReclaim also share this queue, as required by FSKit.
    private func complete<Input, Result>(
        _ reply: @escaping @Sendable (Result?, Error?) -> Void,
        operation: @escaping () async throws -> Input,
        result: @escaping (Input) throws -> Result
    ) {
        state.async {
            guard self.active else { return reply(nil, POSIXError(.ENXIO)) }
            let id = UUID()
            let task = Task {
                do {
                    let value = try await operation()
                    self.state.async {
                        guard self.pending.removeValue(forKey: id) != nil else { return }
                        do {
                            guard self.active else { throw POSIXError(.ENXIO) }
                            reply(try result(value), nil)
                        } catch { reply(nil, error) }
                    }
                } catch {
                    self.state.async {
                        guard self.pending.removeValue(forKey: id) != nil else { return }
                        reply(nil, error)
                    }
                }
            }
            self.pending[id] = Pending(task: task, replyCancelled: { reply(nil, POSIXError(.ENXIO)) })
        }
    }

    func item(_ metadata: SharedFileItem) throws -> SharedFilesItem {
        if let item = items[metadata.id] {
            if item.metadata.kind != metadata.kind ||
               (item.metadata.kind == .file && item.metadata.version != metadata.version) {
                let replacement = SharedFilesItem(metadata)
                retiredItems[ObjectIdentifier(item)] = item
                items[metadata.id] = replacement
                // FSKit may call back into the module while changing caches.
                // Never hold the item-state synchronization queue during this
                // native request; old handles already fail identity checks.
                cacheUpdates.async { [self, item] in
                    _ = setCacheState(for: item, cacheMode: .none, coherencyType: .noCache, action: .revoke)
                }
                return replacement
            }
            item.metadata = metadata
            return item
        }
        let item = SharedFilesItem(metadata)
        items[metadata.id] = item
        return item
    }

    private func metadata(_ item: FSItem) throws -> SharedFileItem {
        try state.sync {
            guard active, let file = item as? SharedFilesItem,
                  items[file.metadata.id] === file else { throw POSIXError(.ESTALE) }
            return file.metadata
        }
    }

    static func attributes(for metadata: SharedFileItem) throws -> FSItem.Attributes {
        let seconds = (metadata.modified ?? Date(timeIntervalSince1970: 0)).timeIntervalSince1970
        guard metadata.size <= UInt64(Int64.max), seconds.isFinite,
              seconds >= Double(Int.min), seconds < Double(Int.max) else { throw POSIXError(.EOVERFLOW) }
        let attributes = FSItem.Attributes()
        attributes.fileID = FSItem.Identifier(metadata.id)
        attributes.parentID = FSItem.Identifier(metadata.parentID)
        attributes.type = metadata.kind == .directory ? .directory : .file
        attributes.uid = getuid()
        attributes.gid = getgid()
        attributes.mode = metadata.kind == .directory ? 0o500 : 0o400 | (metadata.permissions & 0o100)
        attributes.linkCount = 1
        attributes.flags = 0
        attributes.size = metadata.size
        attributes.allocSize = 0
        attributes.supportsLimitedXAttrs = true
        attributes.inhibitKernelOffloadedIO = true
        let time = timespec(tv_sec: Int(floor(seconds)), tv_nsec: Int((seconds - floor(seconds)) * 1_000_000_000))
        attributes.modifyTime = time
        attributes.changeTime = time
        attributes.accessTime = time
        attributes.birthTime = time
        attributes.addedTime = time
        attributes.backupTime = timespec()
        return attributes
    }

    func activateVolume(options: FSTaskOptions,
                        replyHandler reply: @escaping @Sendable (FSActivateResult?, Error?) -> Void) {
        state.async {
            self.active = true
            self.complete(reply, operation: { try await self.client.item(id: 2) }) {
                guard $0.kind == .directory else { throw POSIXError(.ENOTDIR) }
                return try FSActivateResult(rootItem: self.item($0)).unwrap()
            }
        }
    }

    func deactivateVolume(options: FSDeactivateOptions,
                          replyHandler reply: @escaping @Sendable (Error?) -> Void) {
        state.async {
            self.stop()
            for item in self.items.values { self.retiredItems[ObjectIdentifier(item)] = item }
            self.items.removeAll()
            reply(nil)
        }
    }
    func mount(options: FSTaskOptions, replyHandler reply: @escaping @Sendable (Error?) -> Void) {
        complete({ (_: SharedFileItem?, error) in reply(error) },
                 operation: { try await self.client.item(id: 2) }, result: { $0 })
    }
    func unmount(replyHandler reply: @escaping @Sendable () -> Void) {
        state.async { self.stop(); reply() }
    }
    private func stop() {
        active = false
        let cancelled = Array(pending.values)
        pending.removeAll()
        for request in cancelled { request.task.cancel(); request.replyCancelled() }
    }
    func synchronize(flags: FSSyncFlags, replyHandler reply: @escaping @Sendable (Error?) -> Void) { reply(nil) }
    func reclaimItem(_ item: FSItem, replyHandler reply: @escaping @Sendable (Error?) -> Void) {
        state.async {
            _ = item.tryReclaim {
                if let file = item as? SharedFilesItem,
                   self.items[file.metadata.id] === file { self.items.removeValue(forKey: file.metadata.id) }
                self.retiredItems.removeValue(forKey: ObjectIdentifier(item))
            }
            reply(nil)
        }
    }

    func lookupItem(named name: FSFileName, in directory: FSItem, context: FSContext,
                    replyHandler reply: @escaping @Sendable (FSLookupItemResult?, Error?) -> Void) {
        do {
            let parent = try metadata(directory)
            guard parent.kind == .directory else { throw POSIXError(.ENOTDIR) }
            guard let name = name.string, !name.contains("/"), !name.contains("\0") else { throw POSIXError(.EINVAL) }
            complete(reply, operation: {
                if name == "." { return try await self.client.item(id: parent.id) }
                if name == ".." { return try await self.client.item(id: parent.parentID) }
                guard let child = try await self.client.children(id: parent.id).first(where: { Data($0.name.utf8) == Data(name.utf8) }) else {
                    throw POSIXError(.ENOENT)
                }
                return child
            }) {
                try FSLookupItemResult(foundItem: self.item($0), itemName: FSFileName(string: name),
                                       itemAttributes: try Self.attributes(for: $0)).unwrap()
            }
        } catch { reply(nil, error) }
    }

    func getAttributes(_ attributes: FSItem.GetAttributesRequest, of item: FSItem, context: FSContext,
                       replyHandler reply: @escaping @Sendable (FSGetAttributesResult?, Error?) -> Void) {
        do {
            let previous = try metadata(item)
            complete(reply, operation: { try await self.client.item(id: previous.id) }) {
                guard try self.item($0) === item else { throw POSIXError(.ESTALE) }
                return try FSGetAttributesResult(attributes: Self.attributes(for: $0)).unwrap()
            }
        } catch { reply(nil, error) }
    }

    static func directoryVerifier(_ entries: [SharedFileItem]) -> UInt64 {
        var hash: UInt64 = 14_695_981_039_346_656_037
        for entry in entries {
            for byte in Data("\(entry.id):\(entry.name):\(entry.kind):".utf8) + entry.version {
                hash = (hash ^ UInt64(byte)) &* 1_099_511_628_211
            }
        }
        return hash == 0 ? 1 : hash
    }

    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie,
                            verifier: FSDirectoryVerifier, attributes: FSItem.GetAttributesRequest?,
                            packer: FSDirectoryEntryPacker, context: FSContext,
                            replyHandler reply: @escaping @Sendable (FSEnumerateDirectoryResult?, Error?) -> Void) {
        do {
            let parent = try metadata(directory)
            guard parent.kind == .directory else { throw POSIXError(.ENOTDIR) }
            complete(reply, operation: {
                var entries = try await self.client.children(id: parent.id)
                    .sorted { $0.name.utf8.lexicographicallyPrecedes($1.name.utf8) }
                if attributes == nil {
                    let current = try await self.client.item(id: parent.id)
                    let previous = try await self.client.item(id: parent.parentID)
                    entries = [current, previous] + entries
                }
                return entries
            }) { (entries: [SharedFileItem]) in
                let currentVerifier = Self.directoryVerifier(entries)
                guard verifier.rawValue == 0 || verifier.rawValue == currentVerifier else { throw POSIXError(.ESTALE) }
                guard cookie.rawValue <= UInt64(entries.count) else { throw POSIXError(.EINVAL) }
                for index in Int(cookie.rawValue)..<entries.count {
                    let entry = entries[index]
                    let name = attributes == nil && index < 2 ? (index == 0 ? "." : "..") : entry.name
                    guard packer.packEntry(name: FSFileName(string: name),
                        itemType: entry.kind == .directory ? .directory : .file,
                        itemID: FSItem.Identifier(entry.id), nextCookie: FSDirectoryCookie(rawValue: UInt64(index + 1)),
                        attributes: attributes == nil ? nil : try Self.attributes(for: entry)) else { break }
                }
                return try FSEnumerateDirectoryResult(verifier: currentVerifier).unwrap()
            }
        } catch { reply(nil, error) }
    }

    func read(from item: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer,
              replyHandler reply: @escaping @Sendable (FSReadFileResult?, Error?) -> Void) {
        do {
            let file = try metadata(item)
            guard file.kind == .file else { throw POSIXError(.EISDIR) }
            guard offset >= 0, length >= 0 else { throw POSIXError(.EINVAL) }
            complete(reply, operation: {
                try await self.readRange(file, offset: UInt64(offset), length: length) { data, position in
                    try self.state.sync {
                        guard self.active, self.items[file.id] === item else { throw POSIXError(.ESTALE) }
                        try buffer.withUnsafeMutableBytes { bytes in
                            guard position <= bytes.count, data.count <= bytes.count - position else {
                                throw POSIXError(.EIO)
                            }
                            data.copyBytes(to: UnsafeMutableRawBufferPointer(rebasing: bytes[position..<(position + data.count)]))
                        }
                    }
                }
            }) { count in
                try FSReadFileResult(bytesRead: count, itemAttributes: Self.attributes(for: file)).unwrap()
            }
        } catch { reply(nil, error) }
    }

    /// Transport frames are bounded; the file and the kernel-requested range are
    /// not. Each chunk retains the same source version and only requested bytes.
    func readRange(_ file: SharedFileItem, offset: UInt64, length: Int,
                   consume: (Data, Int) throws -> Void) async throws -> Int {
        guard length >= 0, offset <= UInt64(Int64.max),
              UInt64(length) <= UInt64(Int64.max) - offset else { throw POSIXError(.EINVAL) }
        var completed = 0
        repeat {
            try Task.checkCancellation()
            let requested = min(length - completed, SharedFileVolumeClient.maximumReadLength)
            let chunk = try await client.read(id: file.id, offset: offset + UInt64(completed),
                                              length: requested, version: file.version)
            guard chunk.count <= requested else { throw POSIXError(.EPROTO) }
            try Task.checkCancellation()
            try consume(chunk, completed)
            completed += chunk.count
            if chunk.count < requested { break }
        } while completed < length
        return completed
    }

    // Read-through mode keeps revocation and source-version checks authoritative.
    // A previewer can request additional ranges or even every byte of a file.
    func open(_ item: FSItem, modes: FSVolume.OpenModes, cacheMode: FSVolume.DataCacheMode, context: FSContext,
              replyHandler reply: @escaping @Sendable (FSOpenItemResult?, Error?) -> Void) {
        do {
            guard !modes.contains(.write) else { throw POSIXError(.EROFS) }
            let file = try metadata(item)
            complete(reply, operation: { try await self.client.item(id: file.id) }) {
                guard try self.item($0) === item else { throw POSIXError(.ESTALE) }
                return FSOpenItemResult(grantedCoherency: .noCache)
            }
        } catch { reply(nil, error) }
    }
    func close(_ item: FSItem, context: FSContext, replyHandler reply: @escaping @Sendable () -> Void) { reply() }
    func upgrade(_ item: FSItem, cacheMode: FSVolume.DataCacheMode, context: FSContext,
                 replyHandler reply: @escaping @Sendable (FSUpgradeItemResult?, Error?) -> Void) {
        reply(FSUpgradeItemResult(grantedCoherency: .noCache), nil)
    }

    static func allowsAccess(_ file: SharedFileItem, requested: FSVolume.AccessMask, callerUID: Int) -> Bool {
        guard callerUID == Int(getuid()) else { return false }
        var allowed: FSVolume.AccessMask = [.readData, .readAttributes, .readXattr, .readSecurity]
        if file.kind == .directory || file.permissions & 0o100 != 0 { allowed.insert(.execute) }
        return requested.subtracting(allowed).isEmpty
    }

    func checkAccess(to item: FSItem, requestedAccess access: FSVolume.AccessMask, context: FSContext,
                     replyHandler reply: @escaping @Sendable (FSCheckAccessResult?, Error?) -> Void) {
        do {
            let file = try metadata(item)
            reply(FSCheckAccessResult(accessAllowed: Self.allowsAccess(file, requested: access,
                callerUID: context.effectiveUserID)), nil)
        } catch { reply(nil, error) }
    }

    func supportedXattrNames(for item: FSItem) -> [FSFileName] { [FSFileName(string: "com.apple.quarantine")] }
    func getXattr(named name: FSFileName, of item: FSItem, context: FSContext,
                  replyHandler reply: @escaping @Sendable (FSGetXattrResult?, Error?) -> Void) {
        do {
            _ = try metadata(item)
            guard name.string == "com.apple.quarantine" else { throw POSIXError(.ENOATTR) }
            reply(try FSGetXattrResult(xattrValue: quarantine).unwrap(), nil)
        } catch { reply(nil, error) }
    }
    func listXattrs(of item: FSItem, context: FSContext, replyHandler reply: @escaping @Sendable (FSListXattrsResult?, Error?) -> Void) {
        reply(FSListXattrsResult(xattrNames: supportedXattrNames(for: item)), nil)
    }
    func setXattr(named name: FSFileName, to value: Data?, on item: FSItem, policy: FSVolume.SetXattrPolicy,
                  context: FSContext,
                  replyHandler reply: @escaping @Sendable (FSSetXattrResult?, Error?) -> Void) { reply(nil, POSIXError(.EROFS)) }

    func write(contents: Data, to item: FSItem, at offset: off_t,
               replyHandler reply: @escaping @Sendable (FSWriteFileResult?, Error?) -> Void) { reply(nil, POSIXError(.EROFS)) }
    func createItem(named name: FSFileName, type: FSItem.ItemType, in directory: FSItem,
                    attributes: FSItem.SetAttributesRequest, context: FSContext,
                    replyHandler reply: @escaping @Sendable (FSCreateItemResult?, Error?) -> Void) { reply(nil, POSIXError(.EROFS)) }
    func createSymbolicLink(named name: FSFileName, in directory: FSItem,
                            attributes: FSItem.SetAttributesRequest, linkContents: FSFileName, context: FSContext,
                            replyHandler reply: @escaping @Sendable (FSCreateSymlinkResult?, Error?) -> Void) { reply(nil, POSIXError(.EROFS)) }
    func createLink(to item: FSItem, named name: FSFileName, in directory: FSItem, context: FSContext,
                    replyHandler reply: @escaping @Sendable (FSCreateLinkResult?, Error?) -> Void) { reply(nil, POSIXError(.EROFS)) }
    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName,
                    to destinationName: FSFileName, inDirectory destinationDirectory: FSItem,
                    overItem: FSItem?, context: FSContext,
                    replyHandler reply: @escaping @Sendable (FSRenameItemResult?, Error?) -> Void) { reply(nil, POSIXError(.EROFS)) }
    func removeItem(_ item: FSItem, named name: FSFileName, from directory: FSItem, context: FSContext,
                    replyHandler reply: @escaping @Sendable (FSRemoveItemResult?, Error?) -> Void) { reply(nil, POSIXError(.EROFS)) }
    func setAttributes(_ attributes: FSItem.SetAttributesRequest, on item: FSItem, context: FSContext,
                       replyHandler reply: @escaping @Sendable (FSSetAttributesResult?, Error?) -> Void) { reply(nil, POSIXError(.EROFS)) }
    func readSymbolicLink(_ item: FSItem, context: FSContext,
                          replyHandler reply: @escaping @Sendable (FSReadSymlinkResult?, Error?) -> Void) { reply(nil, POSIXError(.ENOTSUP)) }
}

private extension Optional {
    func unwrap() throws -> Wrapped {
        guard let value = self else { throw POSIXError(.EIO) }
        return value
    }
}
