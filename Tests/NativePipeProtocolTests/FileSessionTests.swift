import CNativePipeFileRPC
import Darwin
import Foundation
import XCTest
@testable import NativePipeProtocol

final class FileSessionTests: XCTestCase {
    private enum Opcode { static let echo: UInt16 = 1, sink: UInt16 = 2, info: UInt16 = 3 }

    func testWireConstantsMatchC() {
        XCTAssertEqual(FileSession.Wire.headerSize, Int(NP_FILE2_HEADER))
        XCTAssertEqual(FileSession.Wire.frameMaximum, Int(NP_FILE2_FRAME_MAX))
        XCTAssertEqual(FileSession.Wire.port, NP_FILE2_PORT)
        XCTAssertEqual(Int(FileSession.Wire.version), Int(NP_FILE2_VERSION))
        XCTAssertEqual(FileSession.Wire.endStream, UInt8(NP_FILE2_END_STREAM))
        XCTAssertEqual(FileSession.Wire.maximumInboundStreams, Int(NP_FILE2_MAX_INBOUND_STREAMS))
        XCTAssertEqual(FileSession.FrameType.credit.rawValue, UInt8(NP_FILE2_CREDIT.rawValue))
    }

    func testEchoLargerThanBothWindows() async throws {
        let (host, guest) = try pair(guestServes: true)
        defer { host.close(); guest.close() }
        try await echo(host, size: Int(FileSession.Wire.connectionWindow) * 3 / 2 + 12345, seed: 7)
        try await echo(host, size: 0, seed: 1)
    }

    func testMultiplexedStreamsInBothDirections() async throws {
        let (host, guest) = try pair(hostServes: true, guestServes: true)
        defer { host.close(); guest.close() }
        try await withThrowingTaskGroup(of: Void.self) { group in
            for index in 0..<12 {
                let session = index % 2 == 0 ? host : guest
                group.addTask { try await self.echo(session, size: 1_048_576 + index * 977, seed: UInt8(index + 1)) }
            }
            try await group.waitForAll()
        }
    }

    func testResponseStatusAndNotifications() async throws {
        let notified = AsyncStream<(UInt16, Data)>.makeStream()
        let (host, guest) = try pair(hostServes: true, guestNotification: { notified.continuation.yield(($0, $1)) })
        defer { host.close(); guest.close() }
        let stream = try await guest.request(Opcode.info, body: Data("abc".utf8), end: true)
        XCTAssertEqual(stream.id % 2, 1)
        let response = try await stream.receive()
        XCTAssertEqual(response, .init(kind: .response(status: 42), body: Data("cba".utf8), end: true))
        let finished = try await stream.receive()
        XCTAssertNil(finished)

        try await host.notify(9, body: Data("invalidate".utf8))
        var iterator = notified.stream.makeAsyncIterator()
        let first = await iterator.next()
        XCTAssertEqual(first?.0, 9)
        XCTAssertEqual(first?.1, Data("invalidate".utf8))
    }

    func testUnservedRequestIsCancelled() async throws {
        let (host, guest) = try pair()
        defer { host.close(); guest.close() }
        let stream = try await host.request(Opcode.info, end: true)
        do { _ = try await stream.receive(); XCTFail("Expected cancellation") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .ECANCELED) }
    }

    func testCreditStallsOneStreamWithoutBlockingOthers() async throws {
        let held = HeldStreams()
        let (host, guest) = try pair(guestRequest: { stream, opcode, body in
            if opcode == Opcode.sink { held.hold(stream) } else { Self.serve(stream, opcode, body) }
        })
        defer { host.close(); guest.close() }
        let stream = try await host.request(Opcode.sink)
        let sender = Task { try await stream.send(Data(count: Int(FileSession.Wire.streamWindow) * 4)) }
        try await Task.sleep(nanoseconds: 200_000_000)
        try await echo(host, size: 100_000, seed: 5)
        stream.cancel()
        do { try await sender.value; XCTFail("Expected cancellation") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .ECANCELED) }
        // The responder sees the cancellation; buffered data was dropped.
        let responder = try await held.next()
        do { _ = try await responder.receive(); XCTFail("Expected cancellation") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .ECANCELED) }
    }

    func testDroppingStreamCancelsAndCloseWakesWaiters() async throws {
        let closed = expectation(description: "host closed")
        let (host, guest) = try pair(guestServes: true, hostClose: { _ in closed.fulfill() })
        defer { host.close() }
        do { _ = try await host.request(Opcode.echo) } // dropped unfinished
        try await echo(host, size: 70_000, seed: 9)
        let pending = try await host.request(Opcode.echo)
        _ = try await pending.receive()
        guest.close()
        await fulfillment(of: [closed], timeout: 5)
        do { _ = try await pending.receive(); XCTFail("Expected disconnect") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .ECONNRESET) }
        do { try await pending.send(Data([1])); XCTFail("Expected disconnect") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .ECONNRESET) }
    }

    func testProtocolViolationsCloseTheSession() async throws {
        let violations: [(String, [Data])] = [
            ("bad magic", [Data("NPFR".utf8) + Data(count: 12)]),
            ("same role", [hello(.host)]),
            ("wrong parity", [hello(.guest), FileSession.frame(.request, id: 2, payload: FileSession.opcode(1))]),
            ("decreasing id", [hello(.guest), FileSession.frame(.request, id: 5, payload: FileSession.opcode(1)),
                               FileSession.frame(.request, id: 3, payload: FileSession.opcode(1))]),
            ("unsolicited response", [hello(.guest), FileSession.frame(.response, id: 2, payload: Data(count: 4))]),
            ("flag on credit", [hello(.guest), FileSession.frame(.credit, flags: 1, id: 0, payload: FileSession.encode(1))]),
            ("data after end", [hello(.guest), FileSession.frame(.request, flags: 1, id: 1, payload: FileSession.opcode(1)),
                                FileSession.frame(.data, id: 1, payload: Data([1]))]),
        ]
        for (name, frames) in violations {
            var descriptors: [Int32] = [-1, -1]
            XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors), 0)
            let failure = Failure(), held = HeldStreams()
            // Held streams stay open, so later frames are checked against them.
            let session = FileSession(connection: try SocketConnection(owning: descriptors[0]), role: .host,
                                      onRequest: { stream, _, _ in held.hold(stream) },
                                      onClose: { failure.set($0) })
            session.start()
            let raw = try SocketConnection(owning: descriptors[1])
            for frame in frames { try await raw.write(frame) }
            let error = try await failure.wait()
            XCTAssertEqual((error as? POSIXError)?.code, .EPROTO, name)
            raw.close()
        }
    }

    // MARK: Interoperability with the C implementation

    func testSwiftHostWithCGuestEchoServer() async throws {
        var descriptors: [Int32] = [-1, -1]
        XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors), 0)
        var handlers = np_file2_handlers(context: nil, request: CEcho.request, notify: nil, closed: nil)
        let guest = np_file2_session_start(descriptors[1], NP_FILE2_ROLE_GUEST, &handlers)
        XCTAssertNotNil(guest)
        let host = FileSession(connection: try SocketConnection(owning: descriptors[0]), role: .host)
        host.start()
        try await withThrowingTaskGroup(of: Void.self) { group in
            for index in 0..<4 {
                group.addTask { try await self.echo(host, size: 3_000_000 + index, seed: UInt8(index + 1)) }
            }
            try await group.waitForAll()
        }
        host.close()
        np_file2_session_close(guest)
    }

    func testCHostWithSwiftGuestEchoServer() async throws {
        var descriptors: [Int32] = [-1, -1]
        XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors), 0)
        let guest = FileSession(connection: try SocketConnection(owning: descriptors[1]), role: .guest,
                                onRequest: Self.serve)
        guest.start()
        let size = 5_000_001
        let received: Data = try await withCheckedThrowingContinuation { continuation in
            Thread.detachNewThread {
                continuation.resume(with: Result { try CEcho.roundTrip(socket: descriptors[0], size: size) })
            }
        }
        XCTAssertEqual(received, Self.pattern(size, seed: 3))
        guest.close()
    }

    // MARK: Helpers

    private static func pattern(_ size: Int, seed: UInt8) -> Data {
        Data((0..<size).map { UInt8(truncatingIfNeeded: $0 &* 131) &+ seed })
    }

    private func echo(_ session: FileSession, size: Int, seed: UInt8) async throws {
        let stream = try await session.request(Opcode.echo, body: Data("x".utf8))
        let response = try await stream.receive()
        XCTAssertEqual(response, .init(kind: .response(status: 0), body: Data("echo".utf8), end: false))
        let payload = Self.pattern(size, seed: seed)
        async let sending: Void = stream.send(payload, end: true)
        var received = Data()
        while let message = try await stream.receive() {
            XCTAssertEqual(message.kind, .data)
            XCTAssertLessThanOrEqual(message.body.count, FileSession.Wire.frameMaximum)
            received.append(message.body)
        }
        try await sending
        XCTAssertEqual(received, payload)
    }

    @Sendable private static func serve(_ stream: FileSessionStream, _ opcode: UInt16, _ body: Data) {
        Task {
            switch opcode {
            case Opcode.echo:
                // A requester may abandon the stream at any point.
                try? await stream.respond(body: Data("echo".utf8))
                while let message = try? await stream.receive() { try? await stream.send(message.body) }
                try? await stream.send(Data(), end: true)
            case Opcode.info:
                try? await stream.respond(status: 42, body: Data(body.reversed()), end: true)
                _ = try? await stream.receive()
            default:
                stream.cancel()
            }
        }
    }

    private func pair(hostServes: Bool = false, guestServes: Bool = false,
                      guestRequest: FileSession.RequestHandler? = nil,
                      guestNotification: FileSession.NotificationHandler? = nil,
                      hostClose: FileSession.CloseHandler? = nil) throws -> (FileSession, FileSession) {
        var descriptors: [Int32] = [-1, -1]
        guard socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors) == 0 else { throw POSIXError(.EIO) }
        let host = FileSession(connection: try SocketConnection(owning: descriptors[0]), role: .host,
                               onRequest: hostServes ? Self.serve : nil, onClose: hostClose)
        let guest = FileSession(connection: try SocketConnection(owning: descriptors[1]), role: .guest,
                                onRequest: guestRequest ?? (guestServes ? Self.serve : nil),
                                onNotification: guestNotification)
        host.start(); guest.start()
        return (host, guest)
    }

    private func hello(_ role: FileSession.Role) -> Data {
        FileSession.frame(.hello, id: 0, payload: Data([FileSession.Wire.version, role.rawValue, 0, 0, 0, 0, 0, 0]))
    }

    private final class Failure: @unchecked Sendable {
        private let lock = NSLock()
        private var error: Error?
        private var waiter: CheckedContinuation<Error, Never>?
        func set(_ error: Error) {
            lock.withLock {
                if let waiter { waiter.resume(returning: error); self.waiter = nil } else { self.error = error }
            }
        }
        func wait() async throws -> Error {
            await withCheckedContinuation { continuation in
                lock.withLock {
                    if let error { continuation.resume(returning: error) } else { waiter = continuation }
                }
            }
        }
    }

    private final class HeldStreams: @unchecked Sendable {
        private let lock = NSLock()
        private var streams: [FileSessionStream] = []
        func hold(_ stream: FileSessionStream) { lock.withLock { streams.append(stream) } }
        func next() async throws -> FileSessionStream {
            for _ in 0..<500 {
                if let stream = lock.withLock({ streams.first }) { return stream }
                try await Task.sleep(nanoseconds: 10_000_000)
            }
            throw POSIXError(.ETIMEDOUT)
        }
    }
}

/// The C session's blocking API, driven from plain threads.
private enum CEcho {
    static let request: @convention(c) (UnsafeMutableRawPointer?, OpaquePointer?, UInt16,
                                        UnsafeRawPointer?, Int) -> Void = { _, owned, _, _, _ in
        // The C API is thread-safe; the reference moves to the worker thread.
        nonisolated(unsafe) let stream = owned
        Thread.detachNewThread {
            let message = UnsafeMutablePointer<np_file2_message>.allocate(capacity: 1)
            defer { message.deallocate(); np_file2_stream_release(stream) }
            guard np_file2_respond(stream, 0, "echo", 4, 0) == 0 else { return }
            var result: Int32
            repeat {
                result = np_file2_receive(stream, message)
                if result == 1, np_file2_send(stream, np_file2_message_data(message), Int(message.pointee.length), 0) < 0 {
                    return
                }
            } while result == 1
            if result == 0 { _ = np_file2_send(stream, nil, 0, 1) }
        }
    }

    static func roundTrip(socket: Int32, size: Int) throws -> Data {
        guard let session = np_file2_session_start(socket, NP_FILE2_ROLE_HOST, nil) else { throw POSIXError(.EIO) }
        defer { np_file2_session_close(session) }
        guard let stream = np_file2_request(session, 1, "x", 1, 0) else { throw POSIXError(.EIO) }
        defer { np_file2_stream_release(stream) }
        let message = UnsafeMutablePointer<np_file2_message>.allocate(capacity: 1)
        defer { message.deallocate() }
        guard np_file2_receive(stream, message) == 1, message.pointee.type == UInt8(NP_FILE2_RESPONSE.rawValue)
        else { throw POSIXError(.EPROTO) }
        let payload = (0..<size).map { UInt8(truncatingIfNeeded: $0 &* 131) &+ 3 }
        let sender = Thread {
            payload.withUnsafeBytes { _ = np_file2_send(stream, $0.baseAddress, $0.count, 1) }
        }
        sender.start()
        var received = Data()
        while np_file2_receive(stream, message) == 1 {
            received.append(np_file2_message_data(message), count: Int(message.pointee.length))
        }
        while !sender.isFinished { usleep(1000) }
        return received
    }
}
