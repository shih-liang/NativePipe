import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class ClipboardBridgeTests: XCTestCase {
    func testReadyReplaysExistingSelectionAndHonorsPolicyAndEcho() {
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        pasteboard.setString("before connection", forType: .string)
        let bridge = ClipboardBridge(pasteboard: pasteboard)
        defer { bridge.stop() }
        var offers: [[String]] = []
        var guestRead: UInt32?
        bridge.output = {
            if case .hostSelectionOffered(let types) = $0 { offers.append(types) }
            if case .selectionRequest(let token, _) = $0 { guestRead = token }
        }
        bridge.pollPasteboard()
        XCTAssertTrue(offers.isEmpty)
        bridge.connectionReady()
        XCTAssertEqual(offers.count, 1)
        XCTAssertTrue(offers[0].contains("text/plain"))
        bridge.pollPasteboard()
        XCTAssertEqual(offers.count, 1)
        bridge.guestOffered(mimeTypes: ["text/plain"])
        bridge.guestSuppliedData(token: guestRead!, data: Data("guest selection".utf8))
        bridge.pollPasteboard()
        XCTAssertEqual(offers.count, 1, "Do not echo the guest's own selection")
        bridge.setPolicy(hostToGuest: false, guestToHost: true)
        XCTAssertEqual(offers.last, [])
        bridge.disconnect()
        pasteboard.clearContents()
        pasteboard.setString("while disconnected", forType: .string)
        bridge.pollPasteboard()
        bridge.connectionReady()
        XCTAssertEqual(offers.count, 2, "Disabled sharing sends no reconnect offer")
        bridge.setPolicy(hostToGuest: true, guestToHost: true)
        XCTAssertEqual(offers.count, 3)
        XCTAssertTrue(offers.last!.contains("text/plain"))
    }

    func testGuestFilesTransferOnlyOnPasteAndEachPasteReadsCurrentContents() async throws {
        let directory = URL(fileURLWithPath: "/tmp/clip-" + UUID().uuidString.prefix(12))
        defer { try? FileManager.default.removeItem(at: directory) }
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        let sourceFiles = ClipboardTestFiles(), destinationFiles = ClipboardTestFiles()
        let source = ClipboardBridge(pasteboard: pasteboard, fileDirectory: directory)
        let destination = ClipboardBridge(pasteboard: pasteboard, fileDirectory: directory)
        source.fileAccess = sourceFiles
        destination.fileAccess = destinationFiles
        defer { source.stop(); destination.stop() }
        source.connectionReady()
        var request: UInt32?
        source.output = { if case .selectionRequest(let token, _) = $0 { request = token } }
        source.guestOffered(mimeTypes: ["text/uri-list"])
        source.guestSuppliedData(token: try XCTUnwrap(request), data: FileTransferURLs.encode([
            URL(fileURLWithPath: "/guest/remote.txt"), URL(fileURLWithPath: "/guest/folder", isDirectory: true),
            URL(fileURLWithPath: "/another/remote.txt")
        ]))
        XCTAssertNotNil(ClipboardFileBroker.offer(from: pasteboard))
        XCTAssertFalse((pasteboard.types ?? []).contains(.fileURL), "Do not advertise nonexistent host files")
        XCTAssertEqual(sourceFiles.exports, 0, "Copy fetches names only")
        var types: [String] = []
        destination.output = { if case .hostSelectionOffered(let offered) = $0 { types = offered } }
        destination.connectionReady()
        XCTAssertTrue(types.contains("text/uri-list"))
        XCTAssertEqual(sourceFiles.exports, 0, "Connecting and polling must not start transfer")
        for paste in 1...2 {
            sourceFiles.contents = "paste \(paste)"
            let completed = expectation(description: "paste \(paste)")
            destination.output = {
                if case .hostSelectionData(let token, _, let data) = $0 {
                    XCTAssertEqual(token, UInt32(paste))
                    XCTAssertEqual(try? FileTransferURLs.decode(data ?? Data()).map(\.lastPathComponent).sorted(),
                                   ["folder", "remote.txt", "remote.txt"])
                    completed.fulfill()
                }
            }
            destination.guestRequestedHostData(token: UInt32(paste), mimeType: "text/uri-list")
            await fulfillment(of: [completed], timeout: 5)
            XCTAssertEqual(sourceFiles.exports, paste * 3)
            XCTAssertEqual(destinationFiles.imported.last?.0, "paste \(paste)")
            XCTAssertFalse(destinationFiles.imported.last!.1, "Temporary directories must not become persistent shares")
            XCTAssertFalse(FileManager.default.fileExists(atPath: try XCTUnwrap(destinationFiles.importedFrom).path))
        }
    }

    func testCancelledPasteStopsSourceExportAndCleansStaging() async throws {
        let directory = URL(fileURLWithPath: "/tmp/clip-" + UUID().uuidString.prefix(12))
        defer { try? FileManager.default.removeItem(at: directory) }
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        let access = ClipboardTestFiles()
        access.delay = true
        let source = ClipboardBridge(pasteboard: pasteboard, fileDirectory: directory)
        let destination = ClipboardBridge(pasteboard: pasteboard, fileDirectory: directory)
        source.fileAccess = access
        destination.fileAccess = ClipboardTestFiles()
        defer { source.stop(); destination.stop() }
        source.connectionReady()
        var token: UInt32?
        source.output = { if case .selectionRequest(let id, _) = $0 { token = id } }
        source.guestOffered(mimeTypes: ["text/uri-list"])
        source.guestSuppliedData(token: try XCTUnwrap(token), data: FileTransferURLs.encode([
            URL(fileURLWithPath: "/guest/large.txt")
        ]))
        destination.connectionReady()
        destination.guestRequestedHostData(token: 1, mimeType: "text/uri-list")
        for _ in 0..<200 {
            if access.exportedTo != nil { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        let staging = try XCTUnwrap(access.exportedTo).deletingLastPathComponent().deletingLastPathComponent()
        destination.disconnect()
        for _ in 0..<200 {
            if !FileManager.default.fileExists(atPath: staging.path) { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(access.cancelled)
        XCTAssertFalse(FileManager.default.fileExists(atPath: staging.path))
    }

    func testPasteReportsExportFailureAndRejectsStagingSymlinks() async throws {
        let directory = URL(fileURLWithPath: "/tmp/clip-" + UUID().uuidString.prefix(12)).resolvingSymlinksInPath()
        defer { try? FileManager.default.removeItem(at: directory) }
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        let access = ClipboardTestFiles()
        access.failure = CocoaError(.fileReadNoSuchFile)
        let source = ClipboardFileBroker(directory: directory), destination = ClipboardFileBroker(directory: directory)
        defer { source.stop(); destination.stop() }
        try source.publish([URL(fileURLWithPath: "/guest/deleted.txt")], using: access, to: pasteboard)
        let offer = try XCTUnwrap(ClipboardFileBroker.offer(from: pasteboard))
        do { _ = try await destination.receive(offer); XCTFail("Deleted source must fail the paste") }
        catch { XCTAssertEqual(error.localizedDescription, access.failure!.localizedDescription) }
        let external = directory.appendingPathComponent("external")
        try Data("untouched".utf8).write(to: external)
        let staged = try ClipboardFileBroker.Receipt.create(in: directory, prefix: "p-")
        let parent = staged.directory.appendingPathComponent("0")
        try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: false)
        try FileManager.default.createSymbolicLink(at: parent.appendingPathComponent("file"), withDestinationURL: external)
        XCTAssertThrowsError(try ClipboardFileBroker.Receipt.claim(staged.directory, in: directory, names: ["file"]))
        XCTAssertEqual(try String(contentsOf: external, encoding: .utf8), "untouched")
    }

    func testClaimedFilesSurviveSourceDisconnectAndRejectRevokedCapabilities() async throws {
        let directory = URL(fileURLWithPath: "/tmp/clip-" + UUID().uuidString.prefix(12))
        defer { try? FileManager.default.removeItem(at: directory) }
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        let access = ClipboardTestFiles()
        let source = ClipboardFileBroker(directory: directory), destination = ClipboardFileBroker(directory: directory)
        defer { source.stop(); destination.stop() }
        try source.publish([URL(fileURLWithPath: "/guest/file.txt")], using: access, to: pasteboard)
        let offer = try XCTUnwrap(ClipboardFileBroker.offer(from: pasteboard))
        let wrong = ClipboardFileBroker.Offer(socket: offer.socket, token: UUID(), names: offer.names)
        do { _ = try await destination.receive(wrong); XCTFail("Wrong token must not export files") } catch {}
        XCTAssertEqual(access.exports, 0)
        source.revoke()
        do { _ = try await destination.receive(offer); XCTFail("Revoked offer must not export files") } catch {}
        XCTAssertEqual(access.exports, 0)
        try source.publish([URL(fileURLWithPath: "/guest/file.txt")], using: access, to: pasteboard)
        var receipt: ClipboardFileBroker.Receipt? = try await destination.receive(XCTUnwrap(ClipboardFileBroker.offer(from: pasteboard)))
        let files = try XCTUnwrap(receipt).urls
        let claimed = try XCTUnwrap(receipt).directory
        source.stop()
        XCTAssertEqual(try String(contentsOf: files[0], encoding: .utf8), "copied")
        _ = try await access.importFiles(files, shareDirectories: false)
        receipt = nil
        XCTAssertFalse(FileManager.default.fileExists(atPath: claimed.path))
        let traversal = ClipboardFileBroker.Offer(socket: "../wrong.sock", token: UUID(), names: ["file.txt"])
        do { _ = try await destination.receive(traversal); XCTFail("Do not connect outside the broker directory") } catch {}
    }
}

@MainActor
private final class ClipboardTestFiles: UserFileAccess {
    var imported: [(String, Bool)] = []
    var importedFrom: URL?
    var contents = "copied"
    var failure: Error?
    var exports = 0
    var delay = false
    var cancelled = false
    var exportedTo: URL?
    func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL] {
        for url in urls {
            let file = try url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true
                ? url.appendingPathComponent("child.txt") : url
            imported.append((try String(contentsOf: file, encoding: .utf8), shareDirectories))
        }
        importedFrom = urls.first?.deletingLastPathComponent().deletingLastPathComponent()
        return urls.map { URL(fileURLWithPath: "/destination/" + $0.lastPathComponent) }
    }
    func exportFile(_ remote: URL, to local: URL) async throws {
        exports += 1
        exportedTo = local
        if let failure { throw failure }
        if delay {
            do { try await Task.sleep(for: .seconds(10)) }
            catch { cancelled = true; throw error }
        }
        if remote.hasDirectoryPath {
            try FileManager.default.createDirectory(at: local, withIntermediateDirectories: false)
            try Data(contents.utf8).write(to: local.appendingPathComponent("child.txt"))
        } else { try Data(contents.utf8).write(to: local) }
    }
}
