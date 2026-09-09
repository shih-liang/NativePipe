import Foundation
import XCTest
import NativePipeProtocol

final class LocalSocketTests: XCTestCase {
    @MainActor func testSingleOwnerRestartAndDisconnectedPeers() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let url = directory.appendingPathComponent("test.sock")
        let server = LocalSocketServer(url: url)
        let duplicate = LocalSocketServer(url: url)
        defer { server.stop(); duplicate.stop(); try? FileManager.default.removeItem(at: directory) }
        func start() throws {
            try server.start { handle in
                guard let data = try? handle.read(upToCount: 4), data.count == 4 else { return }
                try? handle.write(contentsOf: data)
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
