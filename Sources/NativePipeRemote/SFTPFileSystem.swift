import Foundation
import Darwin
import NativePipeStrings

public struct SFTPFileEntry: Codable, Sendable, Hashable {
    public enum Kind: String, Codable, Sendable { case file, directory, symbolicLink, other }
    public var name: String
    public var path: String
    public var kind: Kind
    public var size: UInt64?
    public var modified: Date?
    public init(name: String, path: String, kind: Kind, size: UInt64? = nil, modified: Date? = nil) {
        self.name = name; self.path = path; self.kind = kind; self.size = size; self.modified = modified
    }
}

public struct SFTPDirectory: Codable, Sendable, Hashable {
    public var path: String
    public var home: String
    public var entries: [SFTPFileEntry]
    public init(path: String, home: String, entries: [SFTPFileEntry]) { self.path = path; self.home = home; self.entries = entries }
}

public struct SFTPTransferProgress: Codable, Sendable, Hashable {
    public var completedBytes: Int64
    public var totalBytes: Int64?
    public var bytesPerSecond: Double?
    public var isComplete: Bool
    public init(completedBytes: Int64, totalBytes: Int64?, bytesPerSecond: Double? = nil, isComplete: Bool = false) {
        self.completedBytes = completedBytes; self.totalBytes = totalBytes
        self.bytesPerSecond = bytesPerSecond; self.isComplete = isComplete
    }
}

public enum SFTPFileSystemError: LocalizedError {
    case destinationExists(String), invalidPath(String), unsupportedFile(String), sourceChanged(String)
    case localNameCollision(String)
    case replacementRecoveryRequired(destination: String, backup: String)
    case transferCleanupRequired(staging: String, reason: String)
    public var errorDescription: String? {
        switch self {
        case .destinationExists(let path): return NPText("A file or folder already exists at %@.", path)
        case .invalidPath(let path): return NPText("The file path is invalid: %@.", path)
        case .unsupportedFile(let path): return NPText("Symbolic links and special files cannot be transferred: %@.", path)
        case .sourceChanged(let path): return NPText("The source changed during the transfer: %@.", path)
        case .localNameCollision(let path): return NPText("The remote folder contains names that collide on the local filesystem: %@.", path)
        case .replacementRecoveryRequired(let destination, let backup):
            return NPText("Replacement stopped. Check %@ and %@ to recover the original.", destination, backup)
        case .transferCleanupRequired(let staging, let reason):
            return NPText("The transfer stopped. Its temporary data could not be removed from %@: %@", staging, reason)
        }
    }
}

/// Operations run away from the UI's main actor. Browsers may explicitly keep
/// one connection between requests; transfers default to an isolated session.
@MainActor
public final class SFTPFileSystem {
    private let command: SSHCommand
    private let environment: [String: String]
    private let transportFactory: (@Sendable () throws -> SFTPTransport)?
    private let reuseConnection: Bool
    private var active: SFTPTransport?
    private var retained: SFTPFileSession?
    public init(command: SSHCommand, environment: [String: String], reuseConnection: Bool = false) {
        self.command = command; self.environment = environment; transportFactory = nil
        self.reuseConnection = reuseConnection
    }
    init(reuseConnection: Bool = false, transportFactory: @escaping @Sendable () throws -> SFTPTransport) {
        command = SSHCommand(destination: "fixture", application: ["true"])
        environment = [:]; self.transportFactory = transportFactory; self.reuseConnection = reuseConnection
    }
    public func cancel() { active?.cancel() }
    public func close() {
        cancel()
        // The worker retains an active session until it has stopped touching
        // its descriptors. An idle session can be released immediately.
        retained = nil
    }
    private func perform<Value: Sendable>(_ operation: @escaping @Sendable (SFTPConnection) throws -> Value) async throws -> Value {
        guard active == nil else { throw SFTPFailure.message(NPText("A remote file operation is already running.")) }
        try Task.checkCancellation()
        let session = try retained ?? SFTPFileSession(transport: makeTransport())
        let transport = session.transport
        if reuseConnection { retained = session }
        active = transport
        defer { active = nil }
        do {
            return try await withTaskCancellationHandler {
                let worker = Task.detached {
                    let connection: SFTPConnection
                    if let existing = session.connection { connection = existing }
                    else {
                        connection = try SFTPConnection(transport: transport)
                        session.connection = connection
                    }
                    return try operation(connection)
                }
                return try await worker.value
            } onCancel: { transport.cancel() }
        } catch {
            // A failed request may leave replies in flight. Do not reuse its
            // stream or retry a mutation after an ambiguous result.
            if retained === session { retained = nil }
            throw error
        }
    }
    private func makeTransport(cleanup: Bool = false) throws -> SFTPTransport {
        if let transportFactory { return try transportFactory() }
        try command.validate()
        let authenticationDirectory = try SSHCredentialStore.makeAttemptDirectory(environment: environment)
        var childEnvironment = environment
        childEnvironment["NATIVEPIPE_SSH_CONNECTION"] = command.credentialID
        childEnvironment["NATIVEPIPE_SSH_AUTH_SESSION"] = authenticationDirectory.path
        let arguments = Self.subsystemArguments(command: command, environment: environment, cleanup: cleanup)
        if cleanup {
            childEnvironment.removeValue(forKey: "SSH_ASKPASS")
            childEnvironment["SSH_ASKPASS_REQUIRE"] = "never"
        }
        return SFTPSSHTransport(arguments: arguments, environment: childEnvironment,
            authenticationDirectory: authenticationDirectory, timeout: cleanup ? 2 : 45)
    }
    static func subsystemArguments(command: SSHCommand, environment: [String: String], cleanup: Bool) -> [String] {
        ["-T", "-o", "ControlPath=none", "-o", "BatchMode=" + (cleanup ? "yes" : "no"),
         "-o", "ClearAllForwardings=yes", "-o", "ConnectTimeout=" + (cleanup ? "2" : "15"),
         "-o", "ServerAliveInterval=30", "-o", "ServerAliveCountMax=3"]
            + (cleanup ? ["-o", "StrictHostKeyChecking=yes", "-o", "NumberOfPasswordPrompts=0"] : [])
            + (cleanup ? [] : SSHAuthentication.preferredAuthenticationArguments(connection: command.credentialID,
                environment: environment, sshArguments: command.sshArguments))
            + command.sshArguments + ["-s", "--", command.destination, "sftp"]
    }
    private func cleanupRemoteStaging(_ path: String, deadline: TimeInterval) async throws {
        // The canceled SSH process has already closed. A separate task can only
        // remove this operation's unpublished staging, without asking for auth
        // or inheriting cancellation. Bound the entire cleanup, not each file.
        guard deadline > ProcessInfo.processInfo.systemUptime else {
            throw SFTPFailure.message(NPText("The SFTP server did not respond in time."))
        }
        let transport = try makeTransport(cleanup: true)
        do {
            try await Task.detached {
                let remaining = max(0, deadline - ProcessInfo.processInfo.systemUptime)
                let timeout = DispatchWorkItem { transport.cancel() }
                DispatchQueue.global().asyncAfter(deadline: .now() + remaining, execute: timeout)
                defer { timeout.cancel(); transport.close() }
                let connection = try SFTPConnection(transport: transport)
                try SFTPTransferWorker(connection: connection, progress: { _ in })
                    .removeRemoteTree(path, prepareDirectories: true)
            }.value
        } catch {
            if ProcessInfo.processInfo.systemUptime >= deadline {
                throw SFTPFailure.message(NPText("The SFTP server did not respond in time."))
            }
            throw error
        }
    }
    public func list(path: String? = nil) async throws -> SFTPDirectory {
        try await perform { connection in
            let home = try connection.loginHome()
            if let path { try SFTPPath.validate(path) }
            let canonical = try path.map { try connection.realpath($0) } ?? home
            // OPENDIR already rejects non-directories. Its READDIR replies
            // include metadata, so a separate LSTAT is unnecessary here.
            let listing: [(String, SFTPAttributes)]
            do { listing = try connection.entries(canonical) }
            catch let failure as SFTPFailure {
                // OpenSSH's server maps OPENDIR's ENOTDIR to "No such file",
                // which is wrong as well as unhelpful: the path exists. Classify
                // only once that has happened, so a
                // successful listing still costs one request; a directory that
                // fails for another reason (permissions) keeps its own error.
                if case .status = failure, let kind = try? connection.attributes(canonical).kind,
                   kind != .directory {
                    throw SFTPFailure.message(NPText("Choose a remote directory to browse."))
                }
                throw failure
            }
            let entries = try listing.map { name, listed -> SFTPFileEntry in
                let child = try SFTPPath.child(canonical, name)
                let attributes = try listed.hasKind ? listed : connection.attributes(child)
                return SFTPFileEntry(name: name, path: child, kind: attributes.kind, size: attributes.size, modified: attributes.modified)
            }
            return SFTPDirectory(path: canonical, home: home, entries: entries)
        }
    }
    public func stat(path: String) async throws -> SFTPFileEntry {
        try await perform { connection in
            let destination = path == "/" ? "/" : try SFTPPath.destination(path, connection: connection)
            let attributes = try connection.attributes(destination)
            return SFTPFileEntry(name: (destination as NSString).lastPathComponent, path: destination,
                                 kind: attributes.kind, size: attributes.size, modified: attributes.modified)
        }
    }
    public func createDirectory(path: String) async throws {
        try await perform { connection in
            let destination = try SFTPPath.destination(path, connection: connection)
            guard try connection.exists(destination) == nil else { throw SFTPFileSystemError.destinationExists(destination) }
            try connection.mkdir(destination)
        }
    }
    public func transfer(direction: SFTPTransfer.Direction, local: URL, remote: String, recursive: Bool = true,
                         overwrite: Bool = false, progress: @escaping @Sendable (SFTPTransferProgress) -> Void) async throws {
        guard local.isFileURL else { throw SFTPFileSystemError.invalidPath(local.absoluteString) }
        if direction == .download, local.standardizedFileURL.path == "/" { throw SFTPFileSystemError.invalidPath(local.path) }
        do {
            try await perform { connection in
                let worker = SFTPTransferWorker(connection: connection, progress: progress)
                try worker.transfer(direction: direction, local: local, remote: remote, recursive: recursive, overwrite: overwrite)
            }
        } catch let pending as SFTPPendingStagingCleanup {
            do { try await cleanupRemoteStaging(pending.path, deadline: pending.deadline) }
            catch {
                throw SFTPFileSystemError.transferCleanupRequired(staging: pending.path, reason: error.localizedDescription)
            }
            throw pending.original
        }
    }
}

/// Access is serialized by SFTPFileSystem's main-actor active-operation guard.
/// The connection is only read or changed by its single detached worker.
private final class SFTPFileSession: @unchecked Sendable {
    let transport: SFTPTransport
    var connection: SFTPConnection?
    init(transport: SFTPTransport) { self.transport = transport }
    deinit { transport.close() }
}

private struct SFTPPendingStagingCleanup: Error {
    let path: String
    let original: Error
    let deadline: TimeInterval
}

private struct SFTPTreeNode {
    let components: [String]
    let kind: SFTPFileEntry.Kind
    let size: UInt64
    let modified: Date?
    let accessed: Date?
    let permissions: UInt32?
}

private final class SFTPTransferWorker {
    let connection: SFTPConnection
    let progress: @Sendable (SFTPTransferProgress) -> Void
    private var completed: Int64 = 0
    private var total: Int64 = 0
    private var started = ProcessInfo.processInfo.systemUptime
    private var localRootDescriptor: Int32 = -1
    private var lastReportTime: TimeInterval = 0
    private var lastReportedBytes: Int64 = -1
    init(connection: SFTPConnection, progress: @escaping @Sendable (SFTPTransferProgress) -> Void) {
        self.connection = connection; self.progress = progress
    }
    private func report(complete: Bool = false, force: Bool = false) {
        let now = ProcessInfo.processInfo.systemUptime
        let firstAcknowledgment = completed > 0 && lastReportedBytes <= 0
        guard force || complete || lastReportedBytes < 0 || firstAcknowledgment || now - lastReportTime >= 0.1 else { return }
        let elapsed = now - started
        progress(SFTPTransferProgress(completedBytes: completed, totalBytes: total,
            bytesPerSecond: elapsed > 0 && completed > 0 ? Double(completed) / elapsed : nil, isComplete: complete))
        lastReportTime = now; lastReportedBytes = completed
    }
    private func localAttributes(_ url: URL) throws -> SFTPAttributes {
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        var result = SFTPAttributes()
        result.size = (attributes[.size] as? NSNumber)?.uint64Value
        result.modified = attributes[.modificationDate] as? Date
        switch attributes[.type] as? FileAttributeType {
        case .typeRegular: result.permissions = 0o100000
        case .typeDirectory: result.permissions = 0o040000
        case .typeSymbolicLink: result.permissions = 0o120000
        default: result.permissions = 0
        }
        return result
    }
    private func localExists(_ url: URL) throws -> SFTPAttributes? {
        do { return try localAttributes(url) }
        catch let error as NSError where error.domain == NSCocoaErrorDomain && error.code == NSFileReadNoSuchFileError { return nil }
    }
    private func remotePath(_ root: String, _ components: [String]) throws -> String {
        try components.reduce(root) { try SFTPPath.child($0, $1) }
    }
    private func localPath(_ root: URL, _ components: [String]) -> URL {
        components.reduce(root) { $0.appendingPathComponent($1) }
    }
    private func localRawPath(_ root: URL, _ components: [String]) -> String {
        // File URLs on macOS decompose Unicode. Descendant names received from
        // SFTP must keep their original UTF-8 bytes when creating local nodes.
        components.reduce(root.path) { $0 + "/" + $1 }
    }
    private func collect(local: URL?, remote: String?, recursive: Bool) throws -> [SFTPTreeNode] {
        if let local { return try collectLocal(local, recursive: recursive) }
        var nodes: [SFTPTreeNode] = []
        func visit(_ components: [String], listed: SFTPAttributes? = nil) throws {
            try connection.checkCancellation()
            guard components.count <= 128, nodes.count < 100_000 else { throw SFTPFailure.message(NPText("The folder exceeds the supported transfer size or depth.")) }
            let path = try remote.map { try remotePath($0, components) }
            let url = local.map { localPath($0, components) }
            let attributes = try listed.flatMap { $0.hasKind ? $0 : nil } ?? connection.attributes(path!)
            guard attributes.kind == .directory || attributes.kind == .file else {
                throw SFTPFileSystemError.unsupportedFile(url?.path ?? path!)
            }
            let size: UInt64
            if attributes.kind == .file {
                guard let bytes = attributes.size, bytes <= UInt64(Int64.max), total <= Int64.max - Int64(bytes) else {
                    throw SFTPFailure.message(NPText("The file size could not be measured safely."))
                }
                size = bytes; total += Int64(bytes)
            } else { size = 0 }
            nodes.append(SFTPTreeNode(components: components, kind: attributes.kind, size: size, modified: attributes.modified,
                                      accessed: attributes.accessed, permissions: attributes.permissions))
            if attributes.kind == .directory {
                guard recursive else { throw SFTPFailure.message(NPText("Enable recursive transfer to copy a folder.")) }
                for (name, listed) in try connection.entries(path!) {
                    try SFTPPath.validateChild(name); try visit(components + [name], listed: listed)
                }
            }
        }
        try visit([]); return nodes
    }
    private func descriptorAttributes(_ fd: Int32) throws -> SFTPAttributes {
        var value = stat()
        guard fstat(fd, &value) == 0, value.st_size >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        return SFTPAttributes(size: UInt64(value.st_size), permissions: UInt32(value.st_mode),
            modified: Date(timeIntervalSince1970: Double(value.st_mtimespec.tv_sec) + Double(value.st_mtimespec.tv_nsec) / 1_000_000_000),
            accessed: Date(timeIntervalSince1970: Double(value.st_atimespec.tv_sec) + Double(value.st_atimespec.tv_nsec) / 1_000_000_000))
    }
    private func localNames(_ fd: Int32) throws -> [String] {
        let copy = dup(fd)
        guard copy >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        guard let directory = fdopendir(copy) else { Darwin.close(copy); throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { closedir(directory) }
        var result: [String] = []
        while true {
            errno = 0
            guard let entry = readdir(directory) else {
                if errno != 0 { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }; break
            }
            let name = withUnsafePointer(to: &entry.pointee.d_name) { pointer in
                pointer.withMemoryRebound(to: CChar.self, capacity: Int(MAXNAMLEN) + 1) { String(cString: $0) }
            }
            if name == "." || name == ".." { continue }
            try SFTPPath.validateChild(name); result.append(name)
            guard result.count <= 100_000 else { throw SFTPFailure.message(NPText("The folder exceeds the supported transfer size or depth.")) }
        }
        return result.sorted()
    }
    private func collectLocal(_ local: URL, recursive: Bool) throws -> [SFTPTreeNode] {
        localRootDescriptor = Darwin.open(local.path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK)
        guard localRootDescriptor >= 0 else {
            if errno == ELOOP { throw SFTPFileSystemError.unsupportedFile(local.path) }
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        var nodes: [SFTPTreeNode] = []
        func visit(_ fd: Int32, components: [String]) throws {
            try connection.checkCancellation()
            guard components.count <= 128, nodes.count < 100_000 else { throw SFTPFailure.message(NPText("The folder exceeds the supported transfer size or depth.")) }
            let attributes = try descriptorAttributes(fd)
            let path = localPath(local, components).path
            guard attributes.kind == .file || attributes.kind == .directory else { throw SFTPFileSystemError.unsupportedFile(path) }
            let size = attributes.kind == .file ? attributes.size! : 0
            guard size <= UInt64(Int64.max), total <= Int64.max - Int64(size) else { throw SFTPFailure.message(NPText("The file size could not be measured safely.")) }
            total += Int64(size)
            nodes.append(SFTPTreeNode(components: components, kind: attributes.kind, size: size, modified: attributes.modified,
                                      accessed: attributes.accessed, permissions: attributes.permissions))
            if attributes.kind == .directory {
                guard recursive else { throw SFTPFailure.message(NPText("Enable recursive transfer to copy a folder.")) }
                for name in try localNames(fd) {
                    let child = openat(fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK)
                    guard child >= 0 else {
                        if errno == ELOOP { throw SFTPFileSystemError.unsupportedFile(localPath(local, components + [name]).path) }
                        throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
                    }
                    defer { Darwin.close(child) }
                    try visit(child, components: components + [name])
                }
            }
        }
        try visit(localRootDescriptor, components: []); return nodes
    }
    private func openLocalSource(_ components: [String]) throws -> Int32 {
        var fd = dup(localRootDescriptor)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        do {
            for (index, name) in components.enumerated() {
                let child = openat(fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | (index + 1 < components.count ? O_DIRECTORY : 0))
                guard child >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
                Darwin.close(fd); fd = child
            }
            return fd
        } catch { Darwin.close(fd); throw error }
    }
    func transfer(direction: SFTPTransfer.Direction, local: URL, remote: String, recursive: Bool, overwrite: Bool) throws {
        defer { if localRootDescriptor >= 0 { Darwin.close(localRootDescriptor); localRootDescriptor = -1 } }
        do {
            let destination = try SFTPPath.destination(remote, connection: connection)
            let nodes = try collect(local: direction == .upload ? local : nil, remote: direction == .download ? destination : nil, recursive: recursive)
            started = ProcessInfo.processInfo.systemUptime; report()
            if direction == .upload { try upload(nodes, local: local, destination: destination, overwrite: overwrite) }
            else { try download(nodes, source: destination, destination: local, overwrite: overwrite) }
            report(complete: true)
        } catch {
            if lastReportedBytes >= 0, completed > lastReportedBytes { report(force: true) }
            throw error
        }
    }
    private func validateDestination(_ existing: SFTPAttributes?, root: SFTPTreeNode, path: String, overwrite: Bool) throws {
        guard let existing else { return }
        guard overwrite else { throw SFTPFileSystemError.destinationExists(path) }
        guard existing.kind == root.kind, existing.kind == .file || existing.kind == .directory else {
            throw SFTPFailure.message(NPText("Replacement requires a destination of the same file or folder type: %@.", path))
        }
    }
    private func upload(_ nodes: [SFTPTreeNode], local: URL, destination: String, overwrite: Bool) throws {
        let root = nodes[0]
        try validateDestination(connection.exists(destination), root: root, path: destination, overwrite: overwrite)
        let parent = (destination as NSString).deletingLastPathComponent
        let staging = try SFTPPath.child(parent, ".nativepipe-transfer-" + UUID().uuidString)
        do {
            for node in nodes {
                try connection.checkCancellation()
                let target = try remotePath(staging, node.components)
                if node.kind == .directory { try connection.mkdir(target) }
                else { try uploadFile(localPath(local, node.components), destination: target, node: node) }
            }
            for node in nodes.reversed() {
                try connection.setAttributes(remotePath(staging, node.components), permissions: node.permissions,
                    accessed: node.accessed, modified: node.modified)
            }
            try connection.checkCancellation()
            let existing = try connection.exists(destination)
            try validateDestination(existing, root: root, path: destination, overwrite: overwrite)
            if existing == nil { try connection.withPublication { try connection.rename(staging, destination) } }
            else if root.kind == .file, connection.extensions["posix-rename@openssh.com"] == "1" {
                try connection.withPublication { try connection.rename(staging, destination, replacing: true) }
            } else {
                let backup = try SFTPPath.child(parent, ".nativepipe-replaced-" + UUID().uuidString)
                try connection.withPublication {
                    var movedOriginal = false
                    do {
                        try connection.rename(destination, backup); movedOriginal = true
                        try connection.rename(staging, destination)
                    } catch {
                        let publicationError = error
                        // A negative status for the first rename confirms that it
                        // did not move the original. I/O failures are ambiguous.
                        if !movedOriginal, let failure = publicationError as? SFTPFailure, case .status = failure { throw publicationError }
                        do { try connection.rename(backup, destination) }
                        catch { throw SFTPFileSystemError.replacementRecoveryRequired(destination: destination, backup: backup) }
                        throw publicationError
                    }
                }
                // Publication succeeded. Cleanup failure must not roll back a new,
                // complete destination; the original remains in the backup sibling.
                try? removeRemoteTree(backup)
            }
        } catch {
            let original = error
            // An interrupted replacement already carries the original's backup
            // path. Keep that recovery result and its artifacts unchanged.
            if let failure = original as? SFTPFileSystemError,
               case .replacementRecoveryRequired = failure { throw original }
            let deadline = ProcessInfo.processInfo.systemUptime + 3
            let transport = connection.transport
            let timeout = DispatchWorkItem { transport.cancel() }
            DispatchQueue.global().asyncAfter(deadline: .now() + 3, execute: timeout)
            defer { timeout.cancel() }
            do { try removeRemoteTree(staging, prepareDirectories: true) }
            catch { throw SFTPPendingStagingCleanup(path: staging, original: original, deadline: deadline) }
            throw original
        }
    }
    private func uploadFile(_ source: URL, destination: String, node: SFTPTreeNode) throws {
        let fd = try openLocalSource(node.components)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { Darwin.close(fd) }
        var before = stat()
        guard fstat(fd, &before) == 0, (before.st_mode & S_IFMT) == S_IFREG, before.st_size >= 0,
              UInt64(before.st_size) == node.size,
              try descriptorAttributes(fd).modified == node.modified else { throw SFTPFileSystemError.sourceChanged(source.path) }
        let handle = try connection.openFile(destination, writing: true)
        var handleClosed = false
        defer { if !handleClosed { try? connection.closeHandle(handle) } }
        var offset: UInt64 = 0
        var buffer = [UInt8](repeating: 0, count: SFTPWire.chunkSize)
        while offset < node.size {
            var pending: [(UInt32, Int)] = []
            while pending.count < 16, offset < node.size {
                try connection.checkCancellation()
                let count = Darwin.read(fd, &buffer, min(buffer.count, Int(node.size - offset)))
                if count < 0, errno == EINTR { continue }
                guard count > 0 else { throw SFTPFileSystemError.sourceChanged(source.path) }
                pending.append((try connection.enqueueWrite(handle, offset: offset, data: Data(buffer.prefix(count))), count))
                offset += UInt64(count)
            }
            for (id, count) in pending {
                try connection.acknowledgeWrite(id); completed += Int64(count); report()
            }
        }
        var after = stat()
        guard fstat(fd, &after) == 0, after.st_size == before.st_size,
              after.st_mtimespec.tv_sec == before.st_mtimespec.tv_sec,
              after.st_mtimespec.tv_nsec == before.st_mtimespec.tv_nsec else { throw SFTPFileSystemError.sourceChanged(source.path) }
        try connection.closeHandle(handle); handleClosed = true
    }
    fileprivate func removeRemoteTree(_ path: String, depth: Int = 0, prepareDirectories: Bool = false) throws {
        guard depth <= 128 else { throw SFTPFailure.message(NPText("The folder exceeds the supported depth.")) }
        guard let attributes = try connection.exists(path) else { return }
        if attributes.kind == .directory {
            if prepareDirectories { try connection.setAttributes(path, permissions: 0o700, accessed: nil, modified: nil) }
            for (name, _) in try connection.entries(path) {
                try removeRemoteTree(SFTPPath.child(path, name), depth: depth + 1, prepareDirectories: prepareDirectories)
            }
            try connection.remove(path, directory: true)
        } else { try connection.remove(path, directory: false) }
    }
    private func download(_ nodes: [SFTPTreeNode], source: String, destination: URL, overwrite: Bool) throws {
        let root = nodes[0]
        try validateDestination(localExists(destination), root: root, path: destination.path, overwrite: overwrite)
        let parent = destination.deletingLastPathComponent()
        let staging = parent.appendingPathComponent(".nativepipe-transfer-" + UUID().uuidString)
        var published = false
        defer { if !published { try? FileManager.default.removeItem(at: staging) } }
        for node in nodes {
            try connection.checkCancellation()
            let target = localRawPath(staging, node.components)
            if node.kind == .directory {
                guard Darwin.mkdir(target, 0o700) == 0 else {
                    if errno == EEXIST { throw SFTPFileSystemError.localNameCollision(source) }
                    throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
                }
            } else { try downloadFile(remotePath(source, node.components), destination: target, node: node) }
        }
        for node in nodes.reversed() { try setLocalAttributes(localRawPath(staging, node.components), node: node) }
        try connection.checkCancellation()
        let existing = try localExists(destination)
        try validateDestination(existing, root: root, path: destination.path, overwrite: overwrite)
        if existing == nil {
            guard renamex_np(staging.path, destination.path, UInt32(RENAME_EXCL)) == 0 else {
                if errno == EEXIST { throw SFTPFileSystemError.destinationExists(destination.path) }
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
        } else {
            // macOS's swap atomically exchanges both complete trees. The old
            // destination then occupies our staging name and can be removed.
            guard renamex_np(staging.path, destination.path, UInt32(RENAME_SWAP)) == 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            try? FileManager.default.removeItem(at: staging)
        }
        published = true
    }
    private func downloadFile(_ source: String, destination: String, node: SFTPTreeNode) throws {
        guard try connection.attributes(source).kind == .file else { throw SFTPFileSystemError.sourceChanged(source) }
        let handle = try connection.openFile(source, writing: false)
        var handleClosed = false
        defer { if !handleClosed { try? connection.closeHandle(handle) } }
        let before = try connection.fileAttributes(handle)
        guard before.kind == .file, before.size == node.size, before.modified == node.modified else { throw SFTPFileSystemError.sourceChanged(source) }
        let fd = Darwin.open(destination, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0o600)
        guard fd >= 0 else {
            if errno == EEXIST { throw SFTPFileSystemError.localNameCollision(source) }
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        defer { Darwin.close(fd) }
        var offset: UInt64 = 0
        while offset < node.size {
            var pending: [(UInt32, UInt64, Int)] = []
            var requested = offset
            while pending.count < 16, requested < node.size {
                let count = min(SFTPWire.chunkSize, Int(node.size - requested))
                pending.append((try connection.enqueueRead(handle, offset: requested, count: count), requested, count))
                requested += UInt64(count)
            }
            for (id, position, count) in pending {
                guard let first = try connection.receiveRead(id, count: count) else { throw SFTPFileSystemError.sourceChanged(source) }
                let data = first
                // A successful short read is legal in v3. Fill that same range
                // after draining the window so no queued request is stranded.
                if first.count < count {
                    pendingShortReads.append((position + UInt64(first.count), count - first.count))
                }
                try writeLocal(fd, data: data, offset: position)
                completed += Int64(data.count); report()
            }
            for (position, count) in pendingShortReads {
                var remaining = count, position = position
                while remaining > 0 {
                    guard let data = try connection.readFile(handle, offset: position, count: remaining) else { throw SFTPFileSystemError.sourceChanged(source) }
                    try writeLocal(fd, data: data, offset: position)
                    position += UInt64(data.count); remaining -= data.count; completed += Int64(data.count); report()
                }
            }
            pendingShortReads.removeAll(keepingCapacity: true)
            offset = requested
        }
        guard try connection.readFile(handle, offset: offset, count: 1) == nil else { throw SFTPFileSystemError.sourceChanged(source) }
        let after = try connection.fileAttributes(handle)
        guard after.size == before.size, after.modified == before.modified else { throw SFTPFileSystemError.sourceChanged(source) }
        guard fsync(fd) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        try connection.closeHandle(handle); handleClosed = true
    }
    private var pendingShortReads: [(UInt64, Int)] = []
    private func setLocalAttributes(_ path: String, node: SFTPTreeNode) throws {
        let fd = Darwin.open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { Darwin.close(fd) }
        if let permissions = node.permissions, fchmod(fd, mode_t(permissions & 0o777)) != 0 {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        if let modified = node.modified {
            let access = node.accessed ?? modified
            var values = [timeval(tv_sec: Int(access.timeIntervalSince1970), tv_usec: 0),
                          timeval(tv_sec: Int(modified.timeIntervalSince1970), tv_usec: 0)]
            guard futimes(fd, &values) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        }
    }
    private func writeLocal(_ fd: Int32, data: Data, offset: UInt64) throws {
        try data.withUnsafeBytes { bytes in
            var position = 0
            while position < data.count {
                try connection.checkCancellation()
                let count = Darwin.pwrite(fd, bytes.baseAddress!.advanced(by: position), data.count - position, off_t(offset) + off_t(position))
                if count < 0, errno == EINTR { continue }
                guard count > 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
                position += count
            }
        }
    }
}
