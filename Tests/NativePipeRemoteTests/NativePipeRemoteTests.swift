import XCTest
import NativePipeProtocol
@testable import NativePipeRemote
#if canImport(Darwin)
import Darwin
#endif

private final class TestFDQueue: @unchecked Sendable {
    private let lock = NSLock()
    private var descriptors: [Int32]

    init(_ descriptors: [Int32]) {
        self.descriptors = descriptors
    }

    func take() throws -> Int32 {
        lock.lock()
        defer { lock.unlock() }
        guard !descriptors.isEmpty else { throw POSIXError(.EMFILE) }
        return descriptors.removeFirst()
    }

    deinit {
        for descriptor in descriptors { Darwin.close(descriptor) }
    }
}

private final class TestConnectionProbe: @unchecked Sendable {
    private let lock = NSLock()
    private var mainThreadValues: [Bool] = []

    func recordCurrentThread() {
        lock.lock()
        mainThreadValues.append(Thread.isMainThread)
        lock.unlock()
    }

    var openedOnMainThread: Bool {
        lock.lock(); defer { lock.unlock() }
        return mainThreadValues.contains(true)
    }

    var callCount: Int {
        lock.lock(); defer { lock.unlock() }
        return mainThreadValues.count
    }
}

final class NativePipeRemoteTests: XCTestCase {
    private struct SocketPair {
        let session: Int32
        let peer: Int32
    }

    private func socketPair(sendBuffer: Int32? = nil) throws -> SocketPair {
        var descriptors = [Int32](repeating: -1, count: 2)
        guard Darwin.socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors) == 0 else {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        let flags = fcntl(descriptors[0], F_GETFL)
        guard flags >= 0,
              fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == 0 else {
            Darwin.close(descriptors[0])
            Darwin.close(descriptors[1])
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        if var sendBuffer {
            guard setsockopt(
                descriptors[0], SOL_SOCKET, SO_SNDBUF,
                &sendBuffer, socklen_t(MemoryLayout.size(ofValue: sendBuffer))) == 0
            else {
                Darwin.close(descriptors[0])
                Darwin.close(descriptors[1])
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
        }
        return SocketPair(session: descriptors[0], peer: descriptors[1])
    }

    private func readExactly(
        _ count: Int, from fd: Int32, timeout: TimeInterval = 2
    ) throws -> Data {
        var result = Data()
        let deadline = Date().addingTimeInterval(timeout)
        while result.count < count, Date() < deadline {
            var pollDescriptor = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            let ready = Darwin.poll(&pollDescriptor, 1, 20)
            if ready < 0 {
                if errno == EINTR { continue }
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            if ready == 0 { continue }
            if pollDescriptor.revents & Int16(POLLERR | POLLNVAL) != 0 {
                throw POSIXError(.EIO)
            }
            var bytes = [UInt8](repeating: 0, count: count - result.count)
            let received = Darwin.recv(fd, &bytes, bytes.count, MSG_DONTWAIT)
            if received > 0 {
                result.append(contentsOf: bytes.prefix(received))
            } else if received == 0 {
                throw POSIXError(.ECONNRESET)
            } else if errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
        }
        guard result.count == count else { throw POSIXError(.ETIMEDOUT) }
        return result
    }

    private func writeAll(_ data: Data, to fd: Int32) throws {
        try data.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return }
            var written = 0
            while written < raw.count {
                let amount = Darwin.send(
                    fd, base.advanced(by: written), raw.count - written, MSG_NOSIGNAL)
                if amount > 0 {
                    written += amount
                } else if amount < 0 && errno == EINTR {
                    continue
                } else {
                    throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
                }
            }
        }
    }

    private func channelReadyFrame(sessionID: UInt32 = 1) throws -> Data {
        var payload = Data(WindowWire.lifecycleMagic)
        payload.append(contentsOf: [1, 1, 0, 0])
        var littleEndianSession = sessionID.littleEndian
        withUnsafeBytes(of: &littleEndianSession) { payload.append(contentsOf: $0) }
        var littleEndianVersion = WindowWire.windowProtocolVersion.littleEndian
        withUnsafeBytes(of: &littleEndianVersion) { payload.append(contentsOf: $0) }
        return try WireFormat.frame(payload: payload)
    }

    private func framedPayload(from fd: Int32) throws -> Data {
        let header = try readExactly(WireFormat.headerSize, from: fd)
        return try readExactly(try WireFormat.decodeHeader(header), from: fd)
    }

    private func mediaFrame(resourceID: UInt32, bytes: Int) ->
        RemoteMediaReadinessBuffer.Frame {
        let payload = Data(repeating: UInt8(truncatingIfNeeded: resourceID), count: bytes)
        let header = MediaWire.Header(
            surfaceID: 1, resourceID: resourceID, width: 8, height: 8,
            ptsNanos: 0, payloadLength: UInt32(bytes))
        return (header, payload)
    }

    func testRemoteLaneHandshakeUsesOneTokenAndDistinctLaneKinds() {
        let token: UInt64 = 0x0102_0304_0506_0708
        let surface = RemoteLaneHandshake.frame(token: token, lane: .surface)
        let media = RemoteLaneHandshake.frame(token: token, lane: .media)
        XCTAssertEqual(surface.count, RemoteLaneHandshake.byteCount)
        XCTAssertEqual(media.count, RemoteLaneHandshake.byteCount)
        XCTAssertEqual(Data(surface.prefix(4)), Data("NPRH".utf8))
        XCTAssertEqual(surface[4], 1)
        XCTAssertEqual(surface[5], RemoteLaneHandshake.Lane.surface.rawValue)
        XCTAssertEqual(media[5], RemoteLaneHandshake.Lane.media.rawValue)
        XCTAssertEqual(surface.suffix(8), media.suffix(8))
        XCTAssertEqual(Array(surface.suffix(8)), [8, 7, 6, 5, 4, 3, 2, 1])
    }

    func testConnectedSessionWritesSameNonceToDistinctLanes() async throws {
        let surface = try socketPair()
        let media = try socketPair()
        defer {
            Darwin.close(surface.peer)
            Darwin.close(media.peer)
        }
        let descriptors = TestFDQueue([surface.session, media.session])
        let session = RemoteSession(
            host: "test", surfacePort: 1, mediaPort: 2,
            connectionOpener: { _, _, _ in try descriptors.take() })

        try await session.connect()
        let surfaceHello = try readExactly(
            RemoteLaneHandshake.byteCount, from: surface.peer)
        let mediaHello = try readExactly(
            RemoteLaneHandshake.byteCount, from: media.peer)

        XCTAssertEqual(surfaceHello[5], RemoteLaneHandshake.Lane.surface.rawValue)
        XCTAssertEqual(mediaHello[5], RemoteLaneHandshake.Lane.media.rawValue)
        XCTAssertEqual(surfaceHello.suffix(8), mediaHello.suffix(8))
        XCTAssertNotEqual(surfaceHello.suffix(8), Data(repeating: 0, count: 8))
        session.disconnect()
    }

    @MainActor
    func testConnectRunsOffMainActorAndCancellationCannotSucceed() async throws {
        let surface = try socketPair()
        let media = try socketPair()
        defer {
            Darwin.close(surface.peer)
            Darwin.close(media.peer)
        }
        let probe = TestConnectionProbe()
        let mediaOpenStarted = expectation(description: "second lane opener started")
        let cancellationObserved = expectation(description: "connection generation invalidated")
        let releaseMediaOpen = DispatchSemaphore(value: 0)
        let descriptors = TestFDQueue([surface.session, media.session])
        let session = RemoteSession(
            host: "test", surfacePort: 1, mediaPort: 2,
            connectionOpener: { _, port, attempt in
                probe.recordCurrentThread()
                if port == 2 {
                    mediaOpenStarted.fulfill()
                    while attempt.shouldContinue { usleep(1_000) }
                    cancellationObserved.fulfill()
                    releaseMediaOpen.wait()
                }
                return try descriptors.take()
            })
        var reportedConnected = false
        session.onStateChange = { state in
            if state == .connected { reportedConnected = true }
        }

        let task = Task { try await session.connect() }
        await fulfillment(of: [mediaOpenStarted], timeout: 2)
        task.cancel()
        await fulfillment(of: [cancellationObserved], timeout: 2)
        releaseMediaOpen.signal()

        do {
            try await task.value
            XCTFail("a cancelled connection attempt reported success")
        } catch is CancellationError {
            // Expected: installConnectedLanes rejects the invalidated generation
            // even when a connector ignores its cancellation probe.
        } catch {
            XCTFail("unexpected cancellation error: \(error)")
        }
        await Task.yield()
        XCTAssertEqual(probe.callCount, 2)
        XCTAssertFalse(probe.openedOnMainThread)
        XCTAssertFalse(session.isConnected)
        XCTAssertFalse(reportedConnected)
    }

    func testDisconnectInterruptsBlockedWriteAndReplacementGeneration() async throws {
        let firstSurface = try socketPair(sendBuffer: 4096)
        let firstMedia = try socketPair()
        let secondSurface = try socketPair()
        let secondMedia = try socketPair()
        defer {
            Darwin.close(firstSurface.peer)
            Darwin.close(firstMedia.peer)
            Darwin.close(secondSurface.peer)
            Darwin.close(secondMedia.peer)
        }
        let descriptors = TestFDQueue([
            firstSurface.session, firstMedia.session,
            secondSurface.session, secondMedia.session,
        ])
        let session = RemoteSession(
            host: "test", surfacePort: 1, mediaPort: 2,
            connectionOpener: { _, _, _ in try descriptors.take() })

        let firstReady = expectation(description: "first generation ready")
        session.onStateChange = { state in
            if state == .connected { firstReady.fulfill() }
        }
        try await session.connect()
        _ = try readExactly(RemoteLaneHandshake.byteCount, from: firstSurface.peer)
        _ = try readExactly(RemoteLaneHandshake.byteCount, from: firstMedia.peer)
        try writeAll(try channelReadyFrame(), to: firstSurface.peer)
        await fulfillment(of: [firstReady], timeout: 2)

        let largeClipboard = Data(
            repeating: 0xa5, count: WindowWire.maximumClipboardDataSize)
        session.send(.hostSelectionData(
            token: 7, mimeType: "application/octet-stream", data: largeClipboard))
        // Seeing the frame header proves drainWrites entered the large send;
        // leaving the peer unread keeps that nonblocking write back-pressured.
        _ = try readExactly(WireFormat.headerSize, from: firstSurface.peer)
        session.disconnect()

        let secondReady = expectation(description: "replacement generation ready")
        session.onStateChange = { state in
            if state == .connected { secondReady.fulfill() }
        }
        try await session.connect()
        _ = try readExactly(RemoteLaneHandshake.byteCount, from: secondSurface.peer)
        _ = try readExactly(RemoteLaneHandshake.byteCount, from: secondMedia.peer)
        try writeAll(try channelReadyFrame(sessionID: 2), to: secondSurface.peer)
        await fulfillment(of: [secondReady], timeout: 2)

        session.send(.close(window: 99))
        let replacementPayload = try framedPayload(from: secondSurface.peer)
        XCTAssertEqual(
            replacementPayload,
            try WindowWire.commandPayload(for: .close(window: 99)))
        XCTAssertTrue(session.isConnected)
        session.disconnect()
    }

    func testSessionStartsDisconnectedAndDisconnectIsIdempotent() {
        let session = RemoteSession(host: "127.0.0.1", surfacePort: 1, mediaPort: 2)
        XCTAssertFalse(session.isConnected)
        session.disconnect()
        session.disconnect()
        XCTAssertFalse(session.isConnected)
    }

    func testSurfaceHandshakeRequiresReadyAsFirstEvent() throws {
        var handshake = RemoteSurfaceHandshake()
        XCTAssertThrowsError(try handshake.accept(.surfaceCreated(surface: 1))) {
            XCTAssertEqual($0 as? RemoteSessionProtocolError, .expectedChannelReady)
        }
        XCTAssertFalse(handshake.isReady)

        XCTAssertTrue(try handshake.accept(.channelReady(
            sessionID: 7, protocolVersion: WindowWire.windowProtocolVersion)))
        XCTAssertTrue(handshake.isReady)
        XCTAssertFalse(try handshake.accept(.surfaceCreated(surface: 1)))
    }

    func testSurfaceHandshakeRejectsDuplicateReady() throws {
        var handshake = RemoteSurfaceHandshake()
        _ = try handshake.accept(.channelReady(
            sessionID: 7, protocolVersion: WindowWire.windowProtocolVersion))
        XCTAssertThrowsError(try handshake.accept(.channelReady(
            sessionID: 8, protocolVersion: WindowWire.windowProtocolVersion))) {
            XCTAssertEqual($0 as? RemoteSessionProtocolError, .duplicateChannelReady)
        }
    }

    func testMediaThatOutrunsChannelReadyIsReleasedInOrder() throws {
        var buffer = RemoteMediaReadinessBuffer(byteLimit: 1024)
        try buffer.hold([mediaFrame(resourceID: 7, bytes: 3)])
        try buffer.hold([mediaFrame(resourceID: 8, bytes: 4)])
        XCTAssertGreaterThan(buffer.byteCount, 0)

        let released = buffer.releaseAll()
        XCTAssertEqual(released.map { $0.0.resourceID }, [7, 8])
        XCTAssertEqual(released.map { $0.1.count }, [3, 4])
        XCTAssertEqual(buffer.byteCount, 0)
        XCTAssertTrue(buffer.releaseAll().isEmpty)
    }

    func testEarlyMediaBufferFailsClosedWhenPeerFloodsBeforeReady() throws {
        var buffer = RemoteMediaReadinessBuffer(byteLimit: 40)
        XCTAssertThrowsError(try buffer.hold([mediaFrame(resourceID: 7, bytes: 5)])) {
            XCTAssertEqual($0 as? RemoteMediaReadinessError, .bufferLimitExceeded)
        }
        XCTAssertEqual(buffer.byteCount, 0)
    }
}
