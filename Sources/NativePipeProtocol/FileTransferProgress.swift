import Foundation

/// Shared progress for file streams and directory trees. A nil total means the
/// tree is being traversed; completion is reported only after publication.
public struct FileTransferProgress: Codable, Sendable, Hashable {
    public var bytesTransferred: UInt64
    public var totalBytes: UInt64?
    public var relativePath: String
    public var bytesPerSecond: Double?
    public var isComplete: Bool

    public init(bytesTransferred: UInt64, totalBytes: UInt64? = nil, relativePath: String = "",
                bytesPerSecond: Double? = nil, isComplete: Bool = false) {
        self.bytesTransferred = bytesTransferred; self.totalBytes = totalBytes
        self.relativePath = relativePath; self.bytesPerSecond = bytesPerSecond
        self.isComplete = isComplete
    }
}

/// Each underlying stream is serial; callbacks can come from its IO worker.
/// This tracker aggregates successive files without retaining file contents.
public final class FileTransferProgressTracker: @unchecked Sendable {
    private let lock = NSLock()
    private let started = ProcessInfo.processInfo.systemUptime
    private let progress: @Sendable (FileTransferProgress) -> Void
    private var completed: UInt64 = 0
    private var current: UInt64 = 0
    private var total: UInt64?
    private var lastReportTime: TimeInterval = 0
    private var lastPath: String?
    private var lastReportedBytes: UInt64 = 0

    public init(totalBytes: UInt64? = nil, progress: @escaping @Sendable (FileTransferProgress) -> Void) {
        total = totalBytes; self.progress = progress
    }
    public func report(_ bytes: UInt64, relativePath: String) {
        lock.lock()
        current = bytes
        let now = ProcessInfo.processInfo.systemUptime
        let transferred = completed + current
        let firstBytes = transferred > 0 && lastReportedBytes == 0
        guard lastPath != relativePath || firstBytes || now - lastReportTime >= 0.1 else { lock.unlock(); return }
        lastPath = relativePath; lastReportTime = now; lastReportedBytes = transferred
        let sample = value(relativePath: relativePath, complete: false)
        lock.unlock()
        progress(sample)
    }
    public func finishFile() {
        lock.lock()
        completed += current; current = 0
        lastReportedBytes = completed; lastReportTime = ProcessInfo.processInfo.systemUptime
        let sample = value(relativePath: lastPath ?? "", complete: false)
        lock.unlock()
        progress(sample)
    }
    public func finish() {
        lock.lock()
        total = completed + current
        let sample = value(relativePath: "", complete: true)
        lock.unlock()
        progress(sample)
    }
    private func value(relativePath: String, complete: Bool) -> FileTransferProgress {
        let bytes = completed + current
        let elapsed = ProcessInfo.processInfo.systemUptime - started
        return .init(bytesTransferred: bytes, totalBytes: total, relativePath: relativePath,
                     bytesPerSecond: elapsed > 0 && bytes > 0 ? Double(bytes) / elapsed : nil,
                     isComplete: complete)
    }
}
