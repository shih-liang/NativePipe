import Foundation
import Darwin
import XCTest
@testable import NativePipeProtocol

@MainActor
final class ApplicationClientTests: XCTestCase {
    func testCatalogStreamExceedsControlFrameLimitAndRejectsTruncation() async throws {
        let apps = (0..<512).map { index -> GuestApplication in
            var icon = Data(repeating: UInt8(truncatingIfNeeded: index), count: 31_000)
            icon[0] = UInt8(index >> 8)
            return .init(id: "large-\(index).desktop", name: "App \(index)",
                         comment: "\(index):" + String(repeating: "x", count: 4_000), iconData: icon)
        }
        let encoder = PropertyListEncoder()
        encoder.outputFormat = .binary
        XCTAssertGreaterThan(try encoder.encode(apps).count, 16 * 1024 * 1024)
        // Exercise both directions against the existing C-backed codec. This
        // also keeps the >16 MiB catalogue regression on the new async path.
        for asyncWriter in [false, true] {
            var fds: [Int32] = [-1, -1]
            XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &fds), 0)
            let connection = try SocketConnection(owning: fds[0])
            let legacy = FileHandle(fileDescriptor: fds[1], closeOnDealloc: true)
            defer { connection.close(); try? legacy.close() }
            if asyncWriter {
                let receiving = Task.detached {
                    let received = try ApplicationCatalogStream.read(from: legacy)
                    XCTAssertEqual(received, apps)
                    XCTAssertTrue(try ApplicationCatalogStream.read(from: legacy).isEmpty)
                    XCTAssertThrowsError(try ApplicationCatalogStream.read(from: legacy))
                }
                try await ApplicationCatalogStream.write(apps, to: connection)
                try await ApplicationCatalogStream.write([], to: connection)
                connection.finishWriting()
                try await receiving.value
            } else {
                let sending = Task.detached {
                    try ApplicationCatalogStream.write(apps, to: legacy)
                    try ApplicationCatalogStream.write([], to: legacy)
                    try legacy.close()
                }
                let received = try await ApplicationCatalogStream.read(from: connection)
                XCTAssertEqual(received, apps)
                let empty = try await ApplicationCatalogStream.read(from: connection)
                XCTAssertTrue(empty.isEmpty)
                do {
                    _ = try await ApplicationCatalogStream.read(from: connection)
                    XCTFail("EOF without a catalogue terminator must not become an empty list")
                } catch { }
                try await sending.value
            }
        }
    }

    func testCatalogCoalescingBeyond512IconsAndInvalidation() async throws {
        var lists = 0, icons = 0
        var client: ApplicationClient!
        client = ApplicationClient { command in
            guard case .applicationRequest(let token, let action, let id) = command else { return }
            switch action {
            case .list:
                lists += 1
                for page in 0..<19 {
                    client.receive(.batch(token, (0..<32).map {
                        .init(id: "app-\(page * 32 + $0).desktop", name: "Application")
                    }))
                }
                client.receive(.end(token, ""))
            case .icon: icons += 1; client.receive(.icon(token, Data(id.utf8)))
            case .launch, .appearance: client.receive(.launched(token, 0, ""))
            }
        }
        client.setConnected(true)
        let service = client!
        async let first = service.applications()
        async let second = service.applications()
        let values = try await (first, second)
        XCTAssertEqual(values.0.count, 608)
        XCTAssertEqual(values.0, values.1)
        while icons < 608 { await Task.yield() }
        XCTAssertEqual(lists, 1); XCTAssertEqual(icons, 608)
        _ = try await client.applications()
        XCTAssertEqual(lists, 1)
        let revision = client.revision
        client.receive(.changed)
        XCTAssertNil(client.cached)
        XCTAssertNotEqual(client.revision, revision)
        _ = try await client.applications()
        XCTAssertEqual(lists, 2)
        let pid = try await client.launch("app-1.desktop")
        XCTAssertEqual(pid, 0) // D-Bus activation is success, not a fabricated PID.
    }

    func testMetadataDoesNotWaitForIcons() async throws {
        var client: ApplicationClient!
        client = ApplicationClient { command in
            guard case .applicationRequest(let token, .list, _) = command else { return }
            client.receive(.batch(token, [.init(id: "one.desktop", name: "One")]))
            client.receive(.end(token, ""))
        }
        client.setConnected(true)
        let apps = try await client.applications()
        XCTAssertEqual(apps.map(\.name), ["One"])
        XCTAssertNil(apps[0].iconData)
        client.setConnected(false)
    }

    func testDisconnectAndDuplicateEntriesNeverInstallPartialCatalog() async throws {
        var client: ApplicationClient!
        client = ApplicationClient { command in
            guard case .applicationRequest(let token, .list, _) = command else { return }
            let app = GuestApplication(id: "one.desktop", name: "One")
            client.receive(.batch(token, [app, app]))
            client.receive(.end(token, ""))
        }
        client.setConnected(true)
        do { _ = try await client.applications(); XCTFail("duplicate must fail") } catch { }
        XCTAssertNil(client.cached)
        var requestSent = false
        let blocked = ApplicationClient { _ in requestSent = true }
        blocked.setConnected(true)
        let task = Task { try await blocked.applications() }
        while !requestSent { await Task.yield() }
        blocked.setConnected(false)
        do { _ = try await task.value; XCTFail("disconnect must fail") } catch { }
        XCTAssertNil(blocked.cached)
    }

    func testLaunchErrorsAndBinaryValidation() async throws {
        var client: ApplicationClient!
        client = ApplicationClient { command in
            guard case .applicationRequest(let token, .launch, _) = command else { return }
            client.receive(.launched(token, 0, "Application has been removed."))
        }
        client.setConnected(true)
        do { _ = try await client.launch("missing.desktop"); XCTFail("must fail") }
        catch { XCTAssertTrue(error.localizedDescription.contains("removed")) }
        var frame = ApplicationReply.magic + Data([1, 3, 0, 0, 0, 0, 0, 0])
        guard case .changed = try ApplicationReply.decode(frame) else { return XCTFail() }
        frame.append(0)
        XCTAssertThrowsError(try ApplicationReply.decode(frame))
        frame = ApplicationReply.magic + Data([1, 5, 0, 0, 9, 0, 0, 0, 1, 0, 1, 0])
        XCTAssertThrowsError(try ApplicationReply.decode(frame)) // oversized/truncated icon
        let app = GuestApplication(id: "org.gtk.Demo4.desktop", name: "GTK Demo", executable: "gtk4-demo")
        XCTAssertTrue(app.matches(applicationID: "org.gtk.Demo4.App"))
        XCTAssertTrue(app.matches(applicationID: "GTK4-DEMO"))
        XCTAssertFalse(app.matches(applicationID: "org.gtk.Demo40.App"))
    }

    func testBusyResponseFailsOnlyThatRequest() async throws {
        var busy = true
        var client: ApplicationClient!
        client = ApplicationClient { command in
            guard case .applicationRequest(let token, _, _) = command else { return }
            client.receive(busy ? .end(token, "The application service is busy.") : .launched(token, 123, ""))
        }
        client.setConnected(true)
        do { _ = try await client.launch("one.desktop"); XCTFail("must report busy") }
        catch { XCTAssertTrue(error.localizedDescription.contains("busy")) }
        busy = false
        let pid = try await client.launch("one.desktop")
        XCTAssertEqual(pid, 123)
    }
}
