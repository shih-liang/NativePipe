import Foundation
import NativePipeProtocol

/// One connection's receive side. Byte credits and media admission run on the
/// reader; only ordered window/application events enter the main run loop.
final class RemoteInbound: @unchecked Sendable {
    enum Event {
        case packet(RemoteStreamDecoder.Packet)
        case diagnostic(String)
        case ended(Int32)
        case failed(Error)
    }
    private let lock = NSLock()
    private let writer: WindowCommandWriter
    private let media: (@Sendable (MediaWire.Header, Data) -> Bool)?
    private let deliver: @MainActor (Event) -> Void
    private var active = true
    private var scheduled = false
    private var pending: [(event: Event, bytes: Int)] = []
    private var bytes = 0

    init(writer: WindowCommandWriter,
         media: (@Sendable (MediaWire.Header, Data) -> Bool)?,
         deliver: @escaping @MainActor (Event) -> Void) {
        self.writer = writer
        self.media = media
        self.deliver = deliver
    }

    @discardableResult
    func receive(_ packet: RemoteStreamDecoder.Packet, bytes: Int) -> Bool {
        lock.lock()
        guard active else { lock.unlock(); return false }
        switch packet {
        case .acknowledge(let count): writer.acknowledgeRemoteBytes(count, received: true)
        case .credit(let count): writer.acknowledgeRemoteBytes(count, received: false)
        case .media(let header, let data):
            if media?(header, data) == false {
                failLocked(RemoteError.message("Remote media was rejected by the decoder."))
            }
        default: appendLocked(.packet(packet), bytes: bytes)
        }
        let accepting = active
        scheduleLocked()
        lock.unlock()
        return accepting
    }

    func enqueue(_ event: Event, bytes: Int = 0) {
        lock.lock()
        if active { appendLocked(event, bytes: bytes); scheduleLocked() }
        lock.unlock()
    }

    /// Serializes against media admission, so no old reader can enqueue pixels
    /// after the controller clears the frame source for another connection.
    func stop() {
        lock.lock()
        active = false
        pending.removeAll()
        bytes = 0
        lock.unlock()
    }

    private func appendLocked(_ event: Event, bytes count: Int) {
        guard pending.count < 4096, count <= 64 * 1024 * 1024 - bytes else {
            failLocked(RemoteError.message("Remote UI delivery queue exceeded its memory budget."))
            return
        }
        pending.append((event, count)); bytes += count
    }

    private func failLocked(_ error: Error) {
        active = false
        pending = [(.failed(error), 0)]
        bytes = 0
        writer.disconnect()
    }

    private func scheduleLocked() {
        guard !scheduled, !pending.isEmpty else { return }
        scheduled = true
        MainRunLoop.perform { self.drain() }
    }

    @MainActor private func drain() {
        lock.lock()
        let batch = Array(pending.prefix(64))
        pending.removeFirst(batch.count)
        bytes -= batch.reduce(0) { $0 + $1.bytes }
        lock.unlock()
        for item in batch { deliver(item.event) }
        lock.lock()
        scheduled = false
        scheduleLocked()
        lock.unlock()
    }
}
