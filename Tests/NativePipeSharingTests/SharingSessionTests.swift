import Foundation
import NativePipeProtocol
@testable import NativePipeSharing
import XCTest

@MainActor
private final class TransportFixture: UserFileAccess {
    var imported: [[URL]] = []
    var exported: [(URL, URL)] = []
    func importFiles(_ urls: [URL], progress: @escaping @Sendable (FileTransferProgress) -> Void) async throws -> [URL] {
        imported.append(urls)
        return urls.map { URL(fileURLWithPath: "/tmp/nativepipe-drop-test/" + $0.lastPathComponent) }
    }
    func exportFile(_ remote: URL, to local: URL) async throws {
        exported.append((remote, local))
        try Data(remote.path.utf8).write(to: local, options: .withoutOverwriting)
    }
}

@MainActor
private final class VolumeFixture: HostFileVolume {
    var result: Result<[URL], Error>
    var revoked: [GuestFileSharePurpose?] = []
    init(_ result: Result<[URL], Error>) { self.result = result }
    func publish(_ files: [URL], using access: any UserFileAccess, purpose: GuestFileSharePurpose) async throws -> [URL] {
        try result.get()
    }
    func revoke(purpose: GuestFileSharePurpose?) { revoked.append(purpose) }
    func stop() {}
}

@MainActor
final class SharingSessionTests: XCTestCase {
    func testMacFilesUseTheLiveShareFirstAndCopyTheRestInOrder() async throws {
        let transport = TransportFixture()
        let session = SharingSession(transport: transport, guestShare: { url in
            url.lastPathComponent == "live.txt" ? URL(fileURLWithPath: "/run/nativepipe/drops/1/live.txt") : nil
        })
        let urls = ["a.txt", "live.txt", "b.txt"].map { URL(fileURLWithPath: "/Users/me/" + $0) }
        let guest = try await session.guestURLs(for: urls, purpose: .drag)
        XCTAssertEqual(guest.map(\.path), ["/tmp/nativepipe-drop-test/a.txt", "/run/nativepipe/drops/1/live.txt",
                                           "/tmp/nativepipe-drop-test/b.txt"])
        XCTAssertEqual(transport.imported, [[urls[0], urls[2]]], "One copy batch for everything not shared live")
        _ = try await session.guestURLs(for: [urls[1]], purpose: .drag, liveShare: false)
        XCTAssertEqual(transport.imported.last, [urls[1]], "Temporary items are always copied")
    }

    func testOpenWithLinuxAppSharesTheFilesThenLaunchesTheEntryWithTheirURIs() async throws {
        var requests: [(ApplicationAction, String)] = []
        var client: ApplicationClient!
        client = ApplicationClient { command in
            guard case .applicationRequest(let token, let action, let request) = command else { return }
            requests.append((action, request))
            client.receive(.launched(token, 4242, ""))
        }
        client.setConnected(true)
        let session = SharingSession(transport: TransportFixture(), guestShare: { url in
            url.lastPathComponent == "live.txt" ? URL(fileURLWithPath: "/run/nativepipe/drops/1/T/live.txt") : nil
        })
        let files = ["live.txt", "two words.txt"].map { URL(fileURLWithPath: "/Users/me/" + $0) }
        let pid = try await session.open(files, with: "org.gnome.TextEditor.desktop", using: client)
        XCTAssertEqual(pid, 4242)
        XCTAssertEqual(requests.map(\.0), [.open])
        XCTAssertEqual(requests.first?.1, "org.gnome.TextEditor.desktop\nfile:///run/nativepipe/drops/1/T/live.txt\n"
                       + "file:///tmp/nativepipe-drop-test/two%20words.txt")
        do { _ = try await session.open(files, with: "bad/id", using: client); XCTFail("Invalid IDs are refused") }
        catch {}
    }

    func testGuestFilesAreMountedWhenPossibleAndDeferredWhenUnavailable() async throws {
        let remote = URL(fileURLWithPath: "/home/me/file.txt")
        let mounted = URL(fileURLWithPath: "/Volumes/Linux/file.txt")
        let volume = VolumeFixture(.success([mounted]))
        let session = SharingSession(transport: TransportFixture(), hostVolume: volume)
        let shared = try await session.hostFiles(for: [remote], purpose: .open)
        XCTAssertEqual(shared, .mounted([mounted]))
        volume.result = .failure(HostFileVolumeUnavailable("no FSKit"))
        let deferred = try await session.hostFiles(for: [remote], purpose: .open)
        XCTAssertEqual(deferred, .deferred([remote]))
        let none = try await SharingSession(transport: TransportFixture()).hostFiles(for: [remote], purpose: .drag)
        XCTAssertEqual(none, .deferred([remote]))
        volume.result = .failure(POSIXError(.EIO))
        do { _ = try await session.hostFiles(for: [remote], purpose: .open); XCTFail("A real failure is reported") }
        catch { XCTAssertEqual((error as? POSIXError)?.code, .EIO) }
    }

    func testChosenGuestItemsAreCopiedUnderTheirNamesWithoutAVolume() async throws {
        let transport = TransportFixture()
        let session = SharingSession(transport: transport, hostVolume: VolumeFixture(.failure(HostFileVolumeUnavailable("no FSKit"))))
        let remotes = ["/home/me/a/notes.txt", "/home/me/b/notes.txt", "/home/me/c.txt"].map { URL(fileURLWithPath: $0) }
        let urls = try await session.hostURLs(for: remotes, purpose: .files)
        XCTAssertEqual(urls.map(\.lastPathComponent), ["notes.txt", "notes.txt", "c.txt"])
        XCTAssertEqual(urls[0].deletingLastPathComponent(), urls[2].deletingLastPathComponent(), "One folder to show")
        XCTAssertNotEqual(urls[0], urls[1], "Equal names never overwrite each other")
        XCTAssertEqual(try urls.map { try String(contentsOf: $0, encoding: .utf8) }, remotes.map(\.path))
        XCTAssertTrue(session.holdsCopies(for: .files))
        session.revoke(.files)
        XCTAssertFalse(session.holdsCopies())
        let mounted = URL(fileURLWithPath: "/Volumes/Linux/c.txt")
        let live = SharingSession(transport: transport, hostVolume: VolumeFixture(.success([mounted])))
        let shared = try await live.hostURLs(for: [remotes[2]], purpose: .files)
        XCTAssertEqual(shared, [mounted])
        XCTAssertFalse(live.holdsCopies())
        session.stop(); live.stop()
    }

    func testTemporaryCopiesLiveUntilTheirPurposeIsRevoked() async throws {
        let transport = TransportFixture(), volume = VolumeFixture(.success([]))
        let session = SharingSession(transport: transport, hostVolume: volume)
        let clip = try await session.temporaryCopy(of: URL(fileURLWithPath: "/home/me/clip.txt"), purpose: .clipboard)
        let opened = try await session.temporaryCopy(of: URL(fileURLWithPath: "/home/me/open.txt"), purpose: .open)
        XCTAssertEqual(try String(contentsOf: clip, encoding: .utf8), "/home/me/clip.txt")
        session.revoke(.clipboard)
        XCTAssertEqual(volume.revoked, [.clipboard])
        for _ in 0..<100 where FileManager.default.fileExists(atPath: clip.path) { try await Task.sleep(for: .milliseconds(5)) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: clip.path))
        session.revoke()
        try await Task.sleep(for: .milliseconds(20))
        XCTAssertTrue(FileManager.default.fileExists(atPath: opened.path), "An opened copy stays while its app may use it")
        session.stop()
        for _ in 0..<100 where FileManager.default.fileExists(atPath: opened.path) { try await Task.sleep(for: .milliseconds(5)) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: opened.path))
        do { _ = try await session.hostFiles(for: [URL(fileURLWithPath: "/x")], purpose: .drag); XCTFail("Stopped") }
        catch { XCTAssertTrue(error is CancellationError) }
    }
}
