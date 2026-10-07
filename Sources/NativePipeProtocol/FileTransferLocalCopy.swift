import Darwin
import Foundation

private final class NativeCopyContext {
    var code: Int32 = 0
    let sourcePath: String
    let progress: FileTransferProgressTracker
    init(sourcePath: String, progress: FileTransferProgressTracker) {
        self.sourcePath = sourcePath; self.progress = progress
    }
}

public extension FileTransferLocalIO {
    /// Copy a mounted source with native byte-range reads, xattrs and atomic
    /// publication. Existing destinations are never replaced.
    static func copy(source: URL, to destination: URL,
        progress: @escaping @Sendable (FileTransferProgress) -> Void = { _ in }) async throws {
        try await perform {
            guard source.isFileURL, destination.isFileURL,
                  !["", "/", ".", ".."].contains(destination.lastPathComponent) else { throw FileRPC.Failure.invalidPath }
            let staging = destination.deletingLastPathComponent()
                .appendingPathComponent(".nativepipe-drag-" + UUID().uuidString)
            defer { removeCopiedStaging(staging) }
            guard let state = copyfile_state_alloc() else { throw POSIXError(.ENOMEM) }
            defer { copyfile_state_free(state) }
            var metadata = stat()
            guard lstat(source.path, &metadata) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
            guard (metadata.st_mode & S_IFMT) == S_IFREG || (metadata.st_mode & S_IFMT) == S_IFDIR else {
                throw FileRPC.Failure.invalidPath
            }
            let tracker = FileTransferProgressTracker(
                totalBytes: (metadata.st_mode & S_IFMT) == S_IFREG ? UInt64(max(0, metadata.st_size)) : nil,
                progress: progress)
            let context = NativeCopyContext(sourcePath: source.path, progress: tracker)
            defer { withExtendedLifetime(context) {} }
            let callback: copyfile_callback_t = { what, stage, state, source, _, opaque in
                guard let opaque else { return COPYFILE_QUIT }
                let context = Unmanaged<NativeCopyContext>.fromOpaque(opaque).takeUnretainedValue()
                if stage == COPYFILE_ERR { context.code = errno; return COPYFILE_QUIT }
                if Task<Never, Never>.isCancelled { return COPYFILE_QUIT }
                if what == COPYFILE_COPY_DATA || what == COPYFILE_RECURSE_FILE {
                    let path = source.map { String(cString: $0) } ?? context.sourcePath
                    let relative = path.hasPrefix(context.sourcePath + "/")
                        ? String(path.dropFirst(context.sourcePath.count + 1)) : ""
                    var copied: off_t = 0
                    if stage != COPYFILE_START { _ = copyfile_state_get(state, UInt32(COPYFILE_STATE_COPIED), &copied) }
                    context.progress.report(UInt64(max(0, copied)), relativePath: relative)
                    if what == COPYFILE_RECURSE_FILE && stage == COPYFILE_FINISH { context.progress.finishFile() }
                }
                return COPYFILE_CONTINUE
            }
            guard copyfile_state_set(state, UInt32(COPYFILE_STATE_STATUS_CB),
                                    unsafeBitCast(callback, to: UnsafeRawPointer.self)) == 0,
                  copyfile_state_set(state, UInt32(COPYFILE_STATE_STATUS_CTX),
                                    Unmanaged.passUnretained(context).toOpaque()) == 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            let flags = copyfile_flags_t(COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_EXCL | COPYFILE_NOFOLLOW)
            guard copyfile(source.path, staging.path, state, flags) == 0 else {
                if Task<Never, Never>.isCancelled { throw CancellationError() }
                throw POSIXError(POSIXErrorCode(rawValue: context.code == 0 ? errno : context.code) ?? .EIO)
            }
            try Task.checkCancellation()
            guard renamex_np(staging.path, destination.path, UInt32(RENAME_EXCL)) == 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            tracker.finish()
        }
    }

    private static func removeCopiedStaging(_ staging: URL) {
        let manager = FileManager.default
        // copyfile preserves read-only directory modes. Restore write/search
        // on this private failed tree so cleanup can unlink all descendants.
        if let info = try? staging.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey]),
           info.isDirectory == true, info.isSymbolicLink != true {
            try? manager.setAttributes([.posixPermissions: 0o700], ofItemAtPath: staging.path)
            if let entries = manager.enumerator(at: staging, includingPropertiesForKeys: [.isDirectoryKey, .isSymbolicLinkKey]) {
                for case let child as URL in entries {
                    guard let info = try? child.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey]),
                          info.isSymbolicLink != true else { entries.skipDescendants(); continue }
                    if info.isDirectory == true {
                        try? manager.setAttributes([.posixPermissions: 0o700], ofItemAtPath: child.path)
                    }
                }
            }
        }
        try? manager.removeItem(at: staging)
    }

    /// Use only for a newly created copy the caller owns. Cleanup finishes even
    /// if its originating transfer was cancelled; original files are untouched.
    static func removeFailedCopy(at url: URL) async {
        await Task.detached(priority: .utility) { removeCopiedStaging(url) }.value
    }
}
