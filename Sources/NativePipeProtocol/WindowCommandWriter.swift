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
			 .outputsChanged, .windowOutputChanged, .windowState, .inputPreferences,
             .framePresented, .captureFrame, .presentationPause, .presentationResume,
             .selectionRequest, .hostSelectionOffered, .hostSelectionData:
            return .control
        case .keyboardFocus, .key, .pointerEntered, .pointerMoved,
             .pointerLeft, .pointerButton, .pointerScroll,
             .textCommit, .textPreedit, .textDeleteSurrounding, .textEdit:
            return .input
        case .frameReleased, .presentationClockSample, .sceneClockSample,
             .presentationFeedback, .presentationDrain:
            return .feedback
        case .applicationRequest, .fileDrag, .notificationClosed, .notificationAction, .hostOpenResponse:
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
        case payload(Data)
        case fragment(Data)
    }

    private let lane: WindowTransportLane?
    private let lock = NSLock()
    private let queue: DispatchQueue
    private var handle: FileHandle?
    private var pending: [Windowing.HostCommand] = []
    private struct NotificationFeedback {
        let revision: UInt64
        var action: Windowing.HostCommand?
        var closed: Windowing.HostCommand?
    }
    /// The guest admits at most 64 active notifications. Optional feedback
    /// never consumes the lossless window/input/frame-command budget.
    private var guestNotifications: [UInt32: UInt64] = [:]
    private var notificationFeedback: [UInt32: NotificationFeedback] = [:]
    private var notificationOrder: [UInt32] = []
    private static let maximumNotifications = 64
    private var head = 0
    private var writerScheduled = false
    private let remote: Bool
    private var bulk: Data?
    private var bulkOffset = 0
    private var flow = RemoteWire.FlowControl()
    private var receivedBytes = 0
    private var remoteFeedback: [Data] = []

    private var failure: ((Error) -> Void)?
    public var onFailure: ((Error) -> Void)? {
        get { lock.lock(); defer { lock.unlock() }; return failure }
        set { lock.lock(); failure = newValue; lock.unlock() }
    }

    private let write: (FileHandle, Data) throws -> Void

    public init(lane: WindowTransportLane? = nil, remote: Bool = false, write: @escaping (FileHandle, Data) throws -> Void) {
        self.write = write
        self.lane = lane
        self.remote = remote
        self.queue = DispatchQueue(
            label: "com.nativepipe.window.\(String(describing: lane))", qos: .userInteractive)
    }

    public func install(_ handle: FileHandle) {
        lock.lock()
        let old = self.handle
        self.handle = handle
        pending.removeAll(keepingCapacity: true)
        guestNotifications.removeAll(); notificationFeedback.removeAll(); notificationOrder.removeAll()
        head = 0
        writerScheduled = false
        bulk = nil; bulkOffset = 0; flow = .init(); receivedBytes = 0; remoteFeedback.removeAll()
        lock.unlock()
        try? old?.close()
    }

    public func disconnect() {
        lock.lock()
        let old = handle
        handle = nil
        pending.removeAll(keepingCapacity: true)
        guestNotifications.removeAll(); notificationFeedback.removeAll(); notificationOrder.removeAll()
        head = 0
        writerScheduled = false
        bulk = nil; bulkOffset = 0; flow = .init(); receivedBytes = 0; remoteFeedback.removeAll()
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
		if enqueueNotification(command) {
			let shouldSchedule = !writerScheduled && !notificationOrder.isEmpty
			if shouldSchedule { writerScheduled = true }
			lock.unlock()
			if shouldSchedule { queue.async { self.drain() } }
			return
		}
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

    /// Called on the connection's reader before UI delivery. A replacement or
    /// guest withdrawal retires queued responses to the old revision, including
    /// a click whose native callback reaches the main actor late.
    public func observeGuestNotification(_ event: Windowing.GuestEvent) {
        lock.lock(); defer { lock.unlock() }
        guard handle != nil else { return }
        switch event {
        case .channelReady:
            guestNotifications.removeAll(); notificationFeedback.removeAll(); notificationOrder.removeAll()
        case .notificationPosted(let notification):
            observeNotification(id: notification.id, revision: notification.revision)
        case .notificationRejected(let id, let revision):
            if let id, let revision { observeNotification(id: id, revision: revision) }
        case .notificationClosed(let id, let revision):
            if let current = guestNotifications[id], current <= revision {
                guestNotifications[id] = nil
                removeNotificationFeedback(id: id)
            }
        default: break
        }
    }

    private func observeNotification(id: UInt32, revision: UInt64) {
        guard id != 0, revision != 0,
              guestNotifications[id] != nil || guestNotifications.count < Self.maximumNotifications,
              revision >= (guestNotifications[id] ?? 0) else { return }
        if guestNotifications[id] != revision { removeNotificationFeedback(id: id) }
        guestNotifications[id] = revision
    }

    private func removeNotificationFeedback(id: UInt32) {
        notificationFeedback[id] = nil
        notificationOrder.removeAll { $0 == id }
    }

    /// Return true for optional commands, even when refused: they can never
    /// turn a malformed action or pressure burst into a transport failure.
    private func enqueueNotification(_ command: Windowing.HostCommand) -> Bool {
        let id: UInt32, revision: UInt64, action: Bool
        switch command {
        case .notificationAction(let value, let serial, let key):
            guard !key.isEmpty, !key.contains("\0"), key.utf8.count <= WindowWire.maximumNotificationActionSize else { return true }
            id = value; revision = serial; action = true
        case .notificationClosed(let value, let serial, _):
            id = value; revision = serial; action = false
        default: return false
        }
        guard guestNotifications[id] == revision else { return true }
        if notificationFeedback[id] == nil {
            notificationFeedback[id] = .init(revision: revision)
            notificationOrder.append(id)
        }
        if action { notificationFeedback[id]?.action = command }
        else { notificationFeedback[id]?.closed = command }
        return true
    }

    /// Remote transport credits are not Wayland presentation feedback. A
    /// receiver may acknowledge fragments before the complete image exists.
    public func acknowledgeRemoteBytes(_ count: Int, received: Bool) {
        lock.lock()
        guard remote, handle != nil, count > 0,
              received || flow.acknowledge(count, now: ProcessInfo.processInfo.systemUptime) else {
            let callback = failure
            lock.unlock()
            callback?(CocoaError(.coderReadCorrupt))
            return
        }
        if received { receivedBytes += count }
        let schedule = !writerScheduled
        writerScheduled = true
        lock.unlock()
        if schedule { queue.async { self.drain() } }
    }

    public func remotePresentation(surface: UInt32, presentationID: UInt32, displayed: Bool, intervalNanoseconds: UInt32) {
        var payload = Data("NPRP".utf8)
        for value in [surface, presentationID, displayed ? UInt32(1) : 0, intervalNanoseconds] {
            var value = value.littleEndian
            withUnsafeBytes(of: &value) { payload.append(contentsOf: $0) }
        }
        lock.lock()
        guard remote, handle != nil else { lock.unlock(); return }
        guard remoteFeedback.count < Self.maximumPendingCommands else {
            let callback = failure; lock.unlock(); callback?(POSIXError(.ENOBUFS)); return
        }
        remoteFeedback.append(payload)
        let schedule = !writerScheduled
        writerScheduled = true
        lock.unlock()
        if schedule { queue.async { self.drain() } }
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
        guard let current = handle else {
            writerScheduled = false
            lock.unlock()
            return nil
        }
        if remote { compactConsumed() }
        let index = bulk == nil ? (head < pending.count ? head : nil) : pending.firstIndex {
            if case .hostSelectionData = $0 { return false }
            return true
        }
        let outbound: Outbound
        if remote, receivedBytes > 0 {
            outbound = .payload(RemoteWire.acknowledgement(receivedBytes))
            receivedBytes = 0
        } else if !remoteFeedback.isEmpty {
            outbound = .payload(remoteFeedback.removeFirst())
        } else if let index {
            if lane == .feedback, case .frameReleased = pending[head] {
                // NPFT carries only latch/read timing. Calibration, actual
                // outcomes and drain fences are lossless NPW2 records on this
                // same lane; never fold them into a timing batch.
                var end = head + 1
                while end < min(head + 256, pending.count) {
                    guard case .frameReleased = pending[end] else { break }
                    end += 1
                }
                outbound = .feedback(Array(pending[head..<end]))
                head = end
            } else if !remote {
                outbound = .command(pending[head])
                head += 1
            } else {
                outbound = .command(pending.remove(at: index))
            }
            if head == pending.count { pending.removeAll(keepingCapacity: true); head = 0 }
        } else if let id = notificationOrder.first, var feedback = notificationFeedback[id] {
            // A click and its close are a pair. Coalescing the close must not
            // swallow the action, and an action is always written first.
            if let action = feedback.action {
                outbound = .command(action); feedback.action = nil
            } else if let closed = feedback.closed {
                outbound = .command(closed); feedback.closed = nil
                // The guest closes the D-Bus ID on this reply without echoing
                // another wire event. Retire only the revision selected here;
                // a reader may already have observed its replacement.
                if guestNotifications[id] == feedback.revision { guestNotifications[id] = nil }
            } else { preconditionFailure("empty notification feedback") }
            if feedback.action == nil && feedback.closed == nil { removeNotificationFeedback(id: id) }
            else { notificationFeedback[id] = feedback }
        } else if let bytes = bulk, case let count = flow.allowance(remaining: bytes.count - bulkOffset), count > 0 {
            outbound = .fragment(RemoteWire.fragment(bytes, offset: bulkOffset, lane: 2, count: count))
            flow.sent(count, now: ProcessInfo.processInfo.systemUptime)
            bulkOffset += count
            if bulkOffset == bytes.count { bulk = nil; bulkOffset = 0 }
        } else {
            writerScheduled = false
            lock.unlock()
            return nil
        }
        let descriptor = fcntl(current.fileDescriptor, F_DUPFD_CLOEXEC, 0)
        guard descriptor >= 0 else {
            self.handle = nil
            pending.removeAll(keepingCapacity: true)
            guestNotifications.removeAll(); notificationFeedback.removeAll(); notificationOrder.removeAll()
            head = 0
            writerScheduled = false
            let callback = failure
            lock.unlock()
            callback?(POSIXError(.EMFILE))
            return nil
        }
        defer { lock.unlock() }
        let handle = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        return (outbound, handle, current)
    }

    private func drain() {
        while let (outbound, handle, owner) = take() {
            do {
                let payload: Data
                switch outbound {
                case .command(let command):
                    payload = try WindowWire.commandPayload(for: command)
                    if remote, case .hostSelectionData = command, payload.count > RemoteWire.fragmentSize {
                        let record = try WireFormat.frame(payload: payload)
                        lock.lock()
                        if self.handle === owner { bulk = record; bulkOffset = 0 }
                        lock.unlock()
                        continue
                    }
                case .feedback(let commands):
                    guard let encoded = WindowWire.frameTimingPayload(for: commands[...]) else {
                        throw CocoaError(.coderInvalidValue)
                    }
                    payload = encoded
                case .payload(let data): payload = data
                case .fragment(let data):
                    try write(handle, data)
                    continue
                }
                try write(handle, WireFormat.frame(payload: payload))
            } catch {
                lock.lock()
                let isCurrent = self.handle === owner
                let callback = isCurrent ? failure : nil
                if isCurrent {
                    self.handle = nil
                    pending.removeAll(keepingCapacity: true)
                    guestNotifications.removeAll(); notificationFeedback.removeAll(); notificationOrder.removeAll()
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
