import Foundation
import XCTest
import NativePipeProtocol

final class LocalSocketTests: XCTestCase {
    @MainActor func testStopCancelsPartialAndIdlePeersWithoutBlockingHealthyRequest() async throws {
        let directory = URL(fileURLWithPath: "/tmp/ipc-stop-\(UUID().uuidString)")
        let url = directory.appendingPathComponent("socket")
        let server = LocalSocketServer(url: url)
        let started = expectation(description: "All stalled reads started")
        let ended = expectation(description: "All stalled reads cancelled by stop")
        started.expectedFulfillmentCount = 32; ended.expectedFulfillmentCount = 32
        try server.start { connection in
            do {
                let kind = try await connection.readExactly(1, deadline: .now() + .seconds(5))
                if kind == Data([0]) {
                    started.fulfill()
                    defer { ended.fulfill() }
                    _ = try await connection.readExactly(4)
                    XCTFail("A partial request cannot finish")
                } else {
                    try await connection.write(kind, deadline: .now() + .seconds(1))
                }
            } catch { }
        }
        var peers: [SocketConnection] = []
        defer { peers.forEach { $0.close() }; server.stop(); try? FileManager.default.removeItem(at: directory) }
        for index in 0..<32 {
            let peer = try await SocketConnection.connect(to: url)
            peers.append(peer)
            try await peer.write(Data(repeating: 0, count: index.isMultiple(of: 2) ? 1 : 3))
        }
        await fulfillment(of: [started], timeout: 2)
        let healthy = try await SocketConnection.connect(to: url)
        defer { healthy.close() }
        try await healthy.write(Data([42]))
        let response = try await healthy.readExactly(1, deadline: .now() + .seconds(1))
        XCTAssertEqual(response, Data([42]))
        XCTAssertEqual(server.statistics.accepted, 33)
        await server.stopAndWait()
        await fulfillment(of: [ended], timeout: 1)
        XCTAssertFalse(server.isListening)
        for peer in peers {
            let eof = try await peer.read(upToCount: 1, deadline: .now() + .seconds(1))
            XCTAssertTrue(eof.isEmpty)
        }
    }

    @MainActor func testRetiringListenerDoesNotUnlinkReplacementOrTruncateAcknowledgement() async throws {
        let directory = URL(fileURLWithPath: "/tmp/ipc-retire-\(UUID().uuidString)")
        let url = directory.appendingPathComponent("socket")
        let retiring = LocalSocketServer(url: url, maximumConnections: 1)
        let replacement = LocalSocketServer(url: url)
        defer { retiring.stop(); replacement.stop(); try? FileManager.default.removeItem(at: directory) }
        let accepted = expectation(description: "Old listener accepted")
        try retiring.start { connection in
            accepted.fulfill()
            if let data = try? await connection.readExactly(1) { try? await connection.write(data) }
        }
        let old = try await SocketConnection.connect(to: url)
        defer { old.close() }
        await fulfillment(of: [accepted], timeout: 1)
        retiring.stopAccepting()
        try replacement.start { connection in
            if let data = try? await connection.readExactly(1) { try? await connection.write(data) }
        }
        try await old.write(Data([1]))
        let acknowledgement = try await old.readExactly(1, deadline: .now() + .seconds(1))
        XCTAssertEqual(acknowledgement, Data([1]))
        await retiring.stopAndWait()
        let new = try await SocketConnection.connect(to: url)
        defer { new.close() }
        try await new.write(Data([2]))
        let reply = try await new.readExactly(1, deadline: .now() + .seconds(1))
        XCTAssertEqual(reply, Data([2]))
        XCTAssertTrue(replacement.isListening)
    }

    @MainActor func testOneWayRequestsSurviveImmediatePeerClose() async throws {
        let directory = URL(fileURLWithPath: "/tmp").appendingPathComponent(UUID().uuidString)
        let url = directory.appendingPathComponent("oneway.sock")
        let server = LocalSocketServer(url: url)
        let received = expectation(description: "Every buffered request is delivered")
        received.expectedFulfillmentCount = 32
        try server.start { handle in
            defer { handle.close() }
            do {
                let data = try await handle.read(upToCount: 1)
                XCTAssertEqual(data, Data([42]))
                received.fulfill()
            } catch { XCTFail("\(error)") }
        }
        defer { server.stop(); try? FileManager.default.removeItem(at: directory) }
        try await Task.detached {
            for _ in 0..<32 {
                let handle = try LocalSocket.connect(url)
                try handle.write(contentsOf: Data([42]))
                try handle.close()
            }
        }.value
        await fulfillment(of: [received], timeout: 3)
    }

    @MainActor func testSingleOwnerRestartAndDisconnectedPeers() async throws {
        let directory = URL(fileURLWithPath: "/tmp").appendingPathComponent(UUID().uuidString)
        let url = directory.appendingPathComponent("test.sock")
        let server = LocalSocketServer(url: url)
        let duplicate = LocalSocketServer(url: url)
        defer { server.stop(); duplicate.stop(); try? FileManager.default.removeItem(at: directory) }
        func start() throws {
            try server.start { handle in
                guard let data = try? await handle.readExactly(4), data.count == 4 else { return }
                try? await handle.write(data)
            }
        }
        try start()
        XCTAssertThrowsError(try duplicate.start { _ in }) { error in
            XCTAssertEqual((error as? POSIXError)?.code, .EWOULDBLOCK)
        }
        for _ in 0..<2 {
            let echoed = try await Task.detached {
                let handle = try LocalSocket.connect(url)
                defer { try? handle.close() }
                try handle.write(contentsOf: Data([1, 2, 3, 4]))
                return try handle.read(upToCount: 4)
            }.value
            XCTAssertEqual(echoed, Data([1, 2, 3, 4]))
            let departed = try LocalSocket.connect(url)
            try departed.write(contentsOf: Data([4, 3, 2, 1]))
            try departed.close() // Replying must not SIGPIPE the server process.
            server.stop()
            try start()
        }
    }
}
