import Darwin
import Foundation
import XCTest
@testable import NativePipeProtocol

final class SocketConnectionTests: XCTestCase {
    func testAdoptedSocketSuppressesSIGPIPEWithoutPeerConfiguration() async throws {
        var descriptors: [Int32] = [-1, -1]
        XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors), 0)
        let connection = try SocketConnection(owning: descriptors[0])
        defer { connection.close() }
        Darwin.close(descriptors[1])
        do { try await connection.write(Data([1])); XCTFail("Expected a disconnected peer") }
        catch let error as POSIXError { XCTAssertEqual(error.code, .EPIPE) }
        await connection.waitUntilClosed()
    }

    private func pair(maximumQueuedBytes: Int = 64 * 1024 * 1024) throws -> (SocketConnection, SocketConnection) {
        var descriptors: [Int32] = [-1, -1]
        guard socketpair(AF_UNIX, SOCK_STREAM, 0, &descriptors) == 0 else { throw POSIXError(.EIO) }
        for descriptor in descriptors {
            var enabled: Int32 = 1, size: Int32 = 4096
            XCTAssertEqual(setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, 4), 0)
            XCTAssertEqual(setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &size, 4), 0)
        }
        return (try SocketConnection(owning: descriptors[0], maximumQueuedBytes: maximumQueuedBytes),
                try SocketConnection(owning: descriptors[1]))
    }

    func testFragmentedInputSharesOneAbsoluteDeadline() async throws {
        let (reader, writer) = try pair()
        defer { reader.close(); writer.close() }
        let sender = Task {
            for _ in 0..<16 {
                try await writer.write(Data([42]))
                try await Task.sleep(for: .milliseconds(30))
            }
        }
        defer { sender.cancel() }
        let start = ContinuousClock.now
        do {
            _ = try await reader.readExactly(16, deadline: .now() + .milliseconds(150))
            XCTFail("Progress must not extend the original deadline")
        } catch let error as POSIXError { XCTAssertEqual(error.code, .ETIMEDOUT) }
        XCTAssertLessThan(start.duration(to: .now), .seconds(1))
        await reader.waitUntilClosed()
    }

    func testCancellationWakesBlockedReaderAndWriter() async throws {
        let (connection, peer) = try pair()
        defer { connection.close(); peer.close() }
        let ended = expectation(description: "Both directions finish")
        ended.expectedFulfillmentCount = 2
        let read = Task {
            defer { ended.fulfill() }
            do { _ = try await connection.readExactly(1); XCTFail("Read should be cancelled") }
            catch { }
        }
        let write = Task {
            defer { ended.fulfill() }
            do { try await connection.write(Data(repeating: 7, count: 2 * 1024 * 1024)); XCTFail("Write should be cancelled") }
            catch { }
        }
        // Receiving a prefix proves the write has entered the socket, while
        // the remaining output exceeds its deliberately small send buffer.
        _ = try await peer.readExactly(1, deadline: .now() + .seconds(1))
        read.cancel()
        await fulfillment(of: [ended], timeout: 1)
        write.cancel()
        await connection.waitUntilClosed()
    }

    func testBackpressurePreservesBytesOrderAndHalfClose() async throws {
        let (writer, reader) = try pair()
        defer { writer.close(); reader.close() }
        let expected = Data((0..<(2 * 1024 * 1024 + 19)).map { UInt8(truncatingIfNeeded: $0) })
        let deadline = DispatchTime.now() + .seconds(5)
        let sender = Task {
            for offset in stride(from: 0, to: expected.count, by: 32749) {
                try await writer.write(expected.subdata(in: offset..<min(offset + 32749, expected.count)), deadline: deadline)
            }
            writer.finishWriting()
        }
        defer { sender.cancel() }
        var actual = Data()
        while true {
            let data = try await reader.read(upToCount: 11003, deadline: deadline)
            if data.isEmpty { break }
            actual.append(data)
        }
        try await sender.value
        XCTAssertEqual(actual, expected)
        // EOF of one direction does not discard the reverse response.
        try await reader.write(Data([81]), deadline: deadline)
        let reply = try await writer.readExactly(1, deadline: deadline)
        XCTAssertEqual(reply, Data([81]))
    }

    func testOutputLimitRejectsSlowConsumerAndCompletesEverySend() async throws {
        let (writer, peer) = try pair(maximumQueuedBytes: 128 * 1024)
        defer { writer.close(); peer.close() }
        let ended = expectation(description: "Every queued send resolves")
        ended.expectedFulfillmentCount = 128
        for _ in 0..<128 {
            writer.send(Data(repeating: 9, count: 65536)) { result in
                if case .success = result { XCTFail("The peer never drains this output") }
                ended.fulfill()
            }
        }
        await fulfillment(of: [ended], timeout: 1)
        await writer.waitUntilClosed()
    }

    func testDetachLeavesUnreadStreamBytesAndTransfersOwnership() async throws {
        let (connection, peer) = try pair()
        defer { connection.close(); peer.close() }
        try await peer.write(Data([1, 2, 3, 4, 5]))
        let header = try await connection.readExactly(2, deadline: .now() + .seconds(1))
        XCTAssertEqual(header, Data([1, 2]))
        let handle = try await connection.detachFileHandle()
        defer { try? handle.close() }
        connection.close() // Cannot close or shut down the transferred stream.
        XCTAssertEqual(fcntl(handle.fileDescriptor, F_GETFL) & O_NONBLOCK, 0)
        XCTAssertEqual(try handle.read(upToCount: 3), Data([3, 4, 5]))
        try handle.write(contentsOf: Data([6]))
        let reply = try await peer.readExactly(1, deadline: .now() + .seconds(1))
        XCTAssertEqual(reply, Data([6]))
    }
}
