import Foundation
import Darwin

/// Independent host-to-guest vsock execution paths. Keeping the classification
/// exhaustive makes adding a new command an explicit transport decision.
public enum WindowTransportLane: Sendable, Equatable {
    case control
    case input
    case feedback
}

extension Windowing.HostCommand {
    public var transportLane: WindowTransportLane {
        switch self {
		case .configure, .configurePopup, .close, .forceQuit, .dismissPopup, .scaleChanged,
			 .outputsChanged, .windowOutputChanged, .inputPreferences,
             .framePresented, .captureFrame,
             .selectionRequest, .hostSelectionOffered, .hostSelectionData:
            return .control
        case .keyboardFocus, .key, .pointerEntered, .pointerMoved,
             .pointerLeft, .pointerButton, .pointerScroll,
             .textCommit, .textPreedit, .textDeleteSurrounding:
            return .input
        case .frameReleased:
            return .feedback
        case .applicationRequest, .fileDrag:
            return .control
        }
    }
}

/// One serial writer and one lock per transport connection. No lock or kernel
/// credit window is shared with the other execution paths.
public final class WindowCommandWriter: @unchecked Sendable {
	private static let maximumPendingCommands = 65_536
    private enum Outbound {
        case command(Windowing.HostCommand)
        case feedback([Windowing.HostCommand])
    }

    private let lane: WindowTransportLane?
    private let lock = NSLock()
    private let queue: DispatchQueue
    private var handle: FileHandle?
    private var pending: [Windowing.HostCommand] = []
    private var head = 0
    private var writerScheduled = false

    private var failure: ((Error) -> Void)?
    public var onFailure: ((Error) -> Void)? {
        get { lock.lock(); defer { lock.unlock() }; return failure }
        set { lock.lock(); failure = newValue; lock.unlock() }
    }

    private let write: (FileHandle, Data) throws -> Void

    public init(lane: WindowTransportLane? = nil, write: @escaping (FileHandle, Data) throws -> Void) {
        self.write = write
        self.lane = lane
        self.queue = DispatchQueue(
            label: "com.nativepipe.window.\(String(describing: lane))", qos: .userInteractive)
    }

    public func install(_ handle: FileHandle) {
        lock.lock()
        let old = self.handle
        self.handle = handle
        pending.removeAll(keepingCapacity: true)
        head = 0
        writerScheduled = false
        lock.unlock()
        try? old?.close()
    }

    public func disconnect() {
        lock.lock()
        let old = handle
        handle = nil
        pending.removeAll(keepingCapacity: true)
        head = 0
        writerScheduled = false
        lock.unlock()
        try? old?.close()
    }

    public func send(_ command: Windowing.HostCommand) {
        precondition(lane == nil || command.transportLane == lane)
        lock.lock()
        guard handle != nil else {
            lock.unlock()
            return
        }
        compactConsumed()
		guard pending.count < Self.maximumPendingCommands else {
			let callback = failure
			lock.unlock()
			callback?(POSIXError(.ENOBUFS))
			return
		}
		enqueue(command)
        let shouldSchedule = !writerScheduled
        writerScheduled = true
        lock.unlock()
        if shouldSchedule { queue.async { self.drain() } }
    }

    private func enqueue(_ command: Windowing.HostCommand) {
        switch command.transportLane {
        case .control:
            // Replace only adjacent state. Removing an older configure across
            // a framePresented barrier would move the replacement behind that
            // barrier and wake the client before it sees the newest size.
            if let previous = pending.last {
                switch (command, previous) {
                case (.configure(let window, _, _, _),
                      .configure(let oldWindow, _, _, _)) where window == oldWindow:
                    pending[pending.count - 1] = command
                    return
                case (.scaleChanged(let window, _),
                      .scaleChanged(let oldWindow, _)) where window == oldWindow:
                    pending[pending.count - 1] = command
                    return
                case (.outputsChanged, .outputsChanged):
                    pending[pending.count - 1] = command
                    return
                case (.windowOutputChanged(let window, _),
                      .windowOutputChanged(let oldWindow, _)) where window == oldWindow:
                    pending[pending.count - 1] = command
                    return
                case (.inputPreferences, .inputPreferences):
                    pending[pending.count - 1] = command
                    return
                default:
                    break
                }
            }
        case .input:
            if let previous = pending.last {
                switch (command, previous) {
                case (.pointerMoved(let window, _, _),
                      .pointerMoved(let previousWindow, _, _))
                    where window == previousWindow:
                    pending[pending.count - 1] = command
                    return
                case (.pointerScroll, .pointerScroll):
                    if let merged = previous.coalescingScroll(with: command) {
                        pending[pending.count - 1] = merged
                        return
                    }
                default:
                    break
                }
            }
        case .feedback:
            break // buffer-release ids are lossless and remain ordered
        }
        pending.append(command)
    }

    private func compactConsumed() {
        guard head > 0 else { return }
        pending.removeFirst(head)
        head = 0
    }

    private func take() -> (Outbound, FileHandle, FileHandle)? {
        lock.lock()
        guard let current = handle, head < pending.count else {
            pending.removeAll(keepingCapacity: true)
            head = 0
            writerScheduled = false
            lock.unlock()
            return nil
        }
        let descriptor = fcntl(current.fileDescriptor, F_DUPFD_CLOEXEC, 0)
        guard descriptor >= 0 else {
            self.handle = nil
            pending.removeAll(keepingCapacity: true)
            head = 0
            writerScheduled = false
            let callback = failure
            lock.unlock()
            callback?(POSIXError(.EMFILE))
            return nil
        }
        defer { lock.unlock() }
        let handle = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        if lane == .feedback {
            let end = min(head + 256, pending.count)
            let batch = Array(pending[head..<end])
            head = end
            if head == pending.count {
                pending.removeAll(keepingCapacity: true)
                head = 0
            }
            return (.feedback(batch), handle, current)
        }
        let command = pending[head]
        head += 1
        if head == pending.count {
            pending.removeAll(keepingCapacity: true)
            head = 0
        }
        return (.command(command), handle, current)
    }

    private func drain() {
        while let (outbound, handle, owner) = take() {
            do {
                let payload: Data
                switch outbound {
                case .command(let command):
                    payload = try WindowWire.commandPayload(for: command)
                case .feedback(let commands):
                    guard let encoded = WindowWire.frameTimingPayload(for: commands[...]) else {
                        throw CocoaError(.coderInvalidValue)
                    }
                    payload = encoded
                }
                try write(handle, WireFormat.frame(payload: payload))
            } catch {
                lock.lock()
                let isCurrent = self.handle === owner
                let callback = isCurrent ? failure : nil
                if isCurrent {
                    self.handle = nil
                    pending.removeAll(keepingCapacity: true)
                    head = 0
                    writerScheduled = false
                }
                lock.unlock()
                callback?(error)
                return
            }
        }
    }
}
