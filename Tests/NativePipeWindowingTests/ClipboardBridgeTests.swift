import AppKit
import NativePipeProtocol
@testable import NativePipeWindowing
import XCTest

@MainActor
final class ClipboardBridgeTests: XCTestCase {
    func testPublishedGuestSelectionUsesRealMountedURLsWithoutExportingContents() async throws {
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        let files = ClipboardTestFiles()
        let bridge = ClipboardBridge(pasteboard: pasteboard)
        bridge.fileAccess = files
        defer { bridge.stop() }
        let paths = ["/guest/Résumé.txt".precomposedStringWithCanonicalMapping,
                     "/guest/Résumé".decomposedStringWithCanonicalMapping]
        let remotes = [try RemoteFileURL.make(paths[0]), try RemoteFileURL.make(paths[1], isDirectory: true)]
        let mounted = [URL(fileURLWithPath: "/Volumes/Shared/document"), URL(fileURLWithPath: "/Volumes/Shared/folder", isDirectory: true)]
        var published: [URL] = []
        bridge.publishGuestFiles = { urls, access, purpose in
            XCTAssertEqual(purpose, .clipboard)
            XCTAssertTrue((access as AnyObject) === files)
            published = urls
            return mounted
        }
        bridge.connectionReady()
        var token: UInt32?
        bridge.output = { if case .selectionRequest(let value, _) = $0 { token = value } }
        bridge.guestOffered(mimeTypes: ["text/uri-list"])
        bridge.guestSuppliedData(token: try XCTUnwrap(token), data: FileTransferURLs.encode(remotes))
        for _ in 0..<100 {
            if (pasteboard.types ?? []).contains(.fileURL) { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertEqual(published, remotes)
        XCTAssertEqual(published.map { Array($0.path.utf8) }, paths.map { Array($0.utf8) })
        XCTAssertTrue(published[1].hasDirectoryPath)
        XCTAssertEqual(files.exports, 0)
        XCTAssertEqual(pasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL], mounted)
    }

    func testDisconnectDuringPublicationDoesNotOverwriteTheMacClipboard() async throws {
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        let bridge = ClipboardBridge(pasteboard: pasteboard)
        bridge.fileAccess = ClipboardTestFiles()
        defer { bridge.stop() }
        var resume: CheckedContinuation<[URL], Never>?
        bridge.publishGuestFiles = { _, _, _ in await withCheckedContinuation { resume = $0 } }
        bridge.connectionReady()
        pasteboard.clearContents(); pasteboard.setString("keep", forType: .string)
        var token: UInt32?
        bridge.output = { if case .selectionRequest(let value, _) = $0 { token = value } }
        bridge.guestOffered(mimeTypes: ["text/uri-list"])
        bridge.guestSuppliedData(token: try XCTUnwrap(token), data: FileTransferURLs.encode([URL(fileURLWithPath: "/guest/file")]))
        for _ in 0..<100 {
            if resume != nil { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        bridge.disconnect()
        try XCTUnwrap(resume).resume(returning: [URL(fileURLWithPath: "/Volumes/Shared/file")])
        try await Task.sleep(for: .milliseconds(20))
        XCTAssertEqual(pasteboard.string(forType: .string), "keep")
        XCTAssertFalse((pasteboard.types ?? []).contains(.fileURL))
    }

    func testDisablingGuestClipboardRevokesItsPublishedCapabilityOnce() async throws {
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        let bridge = ClipboardBridge(pasteboard: pasteboard)
        bridge.fileAccess = ClipboardTestFiles()
        defer { bridge.stop() }
        var published = false, revocations = 0
        bridge.publishGuestFiles = { _, _, purpose in
            XCTAssertEqual(purpose, .clipboard); published = true
            return [URL(fileURLWithPath: "/Volumes/Shared/file")]
        }
        bridge.onFileSharingRevoked = { revocations += 1 }
        bridge.connectionReady()
        var token: UInt32?
        bridge.output = { if case .selectionRequest(let value, _) = $0 { token = value } }
        bridge.guestOffered(mimeTypes: ["text/uri-list"])
        bridge.guestSuppliedData(token: try XCTUnwrap(token), data: FileTransferURLs.encode([URL(fileURLWithPath: "/guest/file")]))
        for _ in 0..<100 {
            if published { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(published)
        let before = revocations
        bridge.setPolicy(hostToGuest: true, guestToHost: false)
        bridge.setPolicy(hostToGuest: true, guestToHost: false)
        XCTAssertEqual(revocations - before, 1)
    }

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

    func testStandardMountedURLsPasteAcrossMachinesOnlyWhenRequested() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("clipboard-mounted-" + UUID().uuidString)
        let folder = directory.appendingPathComponent("folder", isDirectory: true)
        let file = directory.appendingPathComponent("file.txt")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try Data("folder".utf8).write(to: folder.appendingPathComponent("child.txt"))
        try Data("first".utf8).write(to: file)
        defer { try? FileManager.default.removeItem(at: directory) }
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        let sourceFiles = ClipboardTestFiles(), destinationFiles = ClipboardTestFiles()
        let source = ClipboardBridge(pasteboard: pasteboard), destination = ClipboardBridge(pasteboard: pasteboard)
        source.fileAccess = sourceFiles; destination.fileAccess = destinationFiles
        source.publishGuestFiles = { _, _, purpose in XCTAssertEqual(purpose, .clipboard); return [file, folder] }
        defer { source.stop(); destination.stop() }
        source.connectionReady()
        var request: UInt32?
        source.output = { if case .selectionRequest(let token, _) = $0 { request = token } }
        source.guestOffered(mimeTypes: ["text/uri-list"])
        source.guestSuppliedData(token: try XCTUnwrap(request), data: FileTransferURLs.encode([
            URL(fileURLWithPath: "/guest/file.txt"), URL(fileURLWithPath: "/guest/folder", isDirectory: true)]))
        for _ in 0..<100 {
            if (pasteboard.types ?? []).contains(.fileURL) { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertEqual(pasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL], [file, folder])
        var types: [String] = []
        destination.output = { if case .hostSelectionOffered(let offered) = $0 { types = offered } }
        destination.connectionReady()
        XCTAssertTrue(types.contains("text/uri-list"))
        XCTAssertEqual(sourceFiles.exports, 0); XCTAssertTrue(destinationFiles.imported.isEmpty)
        for paste in 1...2 {
            try Data("paste \(paste)".utf8).write(to: file)
            let completed = expectation(description: "paste \(paste)")
            destination.output = {
                if case .hostSelectionData(let token, _, let data) = $0 {
                    XCTAssertEqual(token, UInt32(paste))
                    XCTAssertEqual(try? FileTransferURLs.decode(data ?? Data()).map(\.lastPathComponent), ["file.txt", "folder"])
                    completed.fulfill()
                }
            }
            destination.guestRequestedHostData(token: UInt32(paste), mimeType: "text/uri-list")
            await fulfillment(of: [completed], timeout: 5)
            XCTAssertEqual(sourceFiles.exports, 0, "No private guest export backend remains")
            XCTAssertEqual(destinationFiles.imported.suffix(2).map(\.0), ["paste \(paste)", "folder"])
            XCTAssertTrue(FileManager.default.fileExists(atPath: file.path), "Pasting never removes the shared source")
        }
    }

    func testMissingPublisherReportsUnavailableWithoutChangingClipboardOrExporting() throws {
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        pasteboard.setString("keep", forType: .string)
        let files = ClipboardTestFiles(), bridge = ClipboardBridge(pasteboard: pasteboard)
        bridge.fileAccess = files
        defer { bridge.stop() }
        bridge.connectionReady()
        var request: UInt32?, failure: Error?
        bridge.output = { if case .selectionRequest(let token, _) = $0 { request = token } }
        bridge.onError = { failure = $0 }
        bridge.guestOffered(mimeTypes: ["text/uri-list"])
        bridge.guestSuppliedData(token: try XCTUnwrap(request), data: FileTransferURLs.encode([URL(fileURLWithPath: "/guest/file")]))
        XCTAssertTrue(failure is GuestFileSharingError)
        XCTAssertEqual(files.exports, 0); XCTAssertEqual(pasteboard.string(forType: .string), "keep")
        XCTAssertFalse((pasteboard.types ?? []).contains(.fileURL))
    }

    func testCancelledPasteCancelsImporterWithoutDeletingSharedSource() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("clipboard-cancel-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        let file = directory.appendingPathComponent("file.txt")
        try Data("keep".utf8).write(to: file)
        defer { try? FileManager.default.removeItem(at: directory) }
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        pasteboard.writeObjects([file as NSURL])
        let files = ClipboardTestFiles(), bridge = ClipboardBridge(pasteboard: pasteboard)
        files.delay = true; bridge.fileAccess = files
        defer { bridge.stop() }
        bridge.connectionReady()
        bridge.guestRequestedHostData(token: 1, mimeType: "text/uri-list")
        for _ in 0..<100 {
            if files.importStarted { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(files.importStarted)
        bridge.disconnect()
        for _ in 0..<100 {
            if files.cancelled { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertTrue(files.cancelled); XCTAssertEqual(files.exports, 0)
        XCTAssertEqual(try Data(contentsOf: file), Data("keep".utf8))
    }

    func testOrdinaryMountedURLReadFailureReportsAndAnswersPaste() async throws {
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        pasteboard.writeObjects([URL(fileURLWithPath: "/nonexistent/linportal-test-" + UUID().uuidString) as NSURL])
        let bridge = ClipboardBridge(pasteboard: pasteboard), files = ClipboardTestFiles()
        bridge.fileAccess = files
        defer { bridge.stop() }
        bridge.connectionReady()
        let completed = expectation(description: "failed paste answered")
        var failure: Error?
        bridge.onError = { failure = $0 }
        bridge.output = { if case .hostSelectionData(_, _, let data) = $0 { XCTAssertNil(data); completed.fulfill() } }
        bridge.guestRequestedHostData(token: 1, mimeType: "text/uri-list")
        await fulfillment(of: [completed], timeout: 5)
        XCTAssertNotNil(failure); XCTAssertEqual(files.exports, 0)
    }
}

@MainActor
private final class ClipboardTestFiles: UserFileAccess {
    var imported: [(String, Bool)] = []
    var exports = 0
    var delay = false
    var importStarted = false
    var cancelled = false
    func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL] {
        importStarted = true
        if delay {
            do { try await Task.sleep(for: .seconds(10)) }
            catch { cancelled = true; throw error }
        }
        let texts = try await FileTransferLocalIO.perform {
            try urls.map { url in
                let file = try url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true
                    ? url.appendingPathComponent("child.txt") : url
                return try String(contentsOf: file, encoding: .utf8)
            }
        }
        imported += texts.map { ($0, shareDirectories) }
        return urls.map { URL(fileURLWithPath: "/destination/" + $0.lastPathComponent) }
    }
    func exportFile(_ remote: URL, to local: URL) async throws {
        exports += 1
        XCTFail("File sharing must never export a private copy")
        throw FileRPC.Failure.protocolError
    }
}
