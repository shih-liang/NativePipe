import AppKit
import NativePipeProtocol
import XCTest
@testable import NativePipeWindowing

@MainActor
private final class DragRecordingView: NSView {
    var draggedItems: [NSDraggingItem] = []
    override func beginDraggingSession(with items: [NSDraggingItem], event: NSEvent, source: any NSDraggingSource) -> NSDraggingSession {
        draggedItems = items
        return NSDraggingSession()
    }
}

@MainActor
private final class DragRangeFiles: UserFileAccess {
    var exports = 0
    func importFiles(_ urls: [URL], shareDirectories: Bool) async throws -> [URL] { urls }
    func exportFile(_ remote: URL, to local: URL) async throws {
        exports += 1
    }
}

@MainActor
final class FileDragBridgeTests: XCTestCase {
    private func event() throws -> NSEvent {
        try XCTUnwrap(NSEvent.mouseEvent(with: .leftMouseDragged, location: .zero, modifierFlags: [],
            timestamp: 0, windowNumber: 0, context: nil, eventNumber: 1, clickCount: 1, pressure: 1))
    }
    private func until(_ condition: () -> Bool) async throws {
        for _ in 0..<100 {
            if condition() { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        throw POSIXError(.ETIMEDOUT)
    }
    private func offer(_ drag: FileDragBridge, token: UInt32 = 1) throws {
        drag.receive(.init(.offered, token: token))
        drag.receive(.init(.sourceData, token: token,
            data: FileTransferURLs.encode([try RemoteFileURL.make("/guest/folder.txt")])))
    }

    func testDirectoryURIWithoutSlashWaitsForMetadataThenDragsResolvedURL() async throws {
        _ = NSApplication.shared
        let bridge = WindowBridge(frameSource: nil), files = DragRangeFiles()
        bridge.fileAccess = files
        let drag = FileDragBridge(bridge: bridge, reportError: { _ in XCTFail("Expected successful publication") })
        defer { drag.disconnect(); bridge.clipboard.stop() }
        var release: CheckedContinuation<[URL], Never>?
        bridge.publishGuestFiles = { urls, _, purpose in
            XCTAssertEqual(urls.map(\.path), ["/guest/folder.txt"])
            XCTAssertFalse(urls[0].hasDirectoryPath); XCTAssertEqual(purpose, .drag)
            return await withCheckedContinuation { release = $0 }
        }
        try offer(drag)
        try await until { release != nil }
        let view = DragRecordingView()
        XCTAssertFalse(drag.beginExportIfNeeded(view: view, event: try event()))
        XCTAssertTrue(view.draggedItems.isEmpty)
        let mountedFolder = URL(fileURLWithPath: "/Volumes/Test Shared/folder.txt", isDirectory: true)
        try XCTUnwrap(release).resume(returning: [mountedFolder])
        // This test has no physically pressed button: a delayed callback must
        // never start a surprise drag. Start a new source gesture whose mount
        // is ready and inspect its writers without posting any OS input.
        try await Task.sleep(for: .milliseconds(20))
        XCTAssertTrue(view.draggedItems.isEmpty)
        bridge.publishGuestFiles = { _, _, _ in [mountedFolder] }
        try offer(drag, token: 2)
        try await Task.sleep(for: .milliseconds(20))
        XCTAssertTrue(drag.beginExportIfNeeded(view: view, event: try event()))
        let writer = try XCTUnwrap(view.draggedItems.first?.item as? NSURL)
        XCTAssertEqual(writer as URL, mountedFolder); XCTAssertTrue((writer as URL).hasDirectoryPath)
        XCTAssertFalse(view.draggedItems.first?.item is NSFilePromiseProvider)
        XCTAssertEqual(files.exports, 0)
    }

    func testMouseUpWhileMountWaitsDiscardsLatePublication() async throws {
        let bridge = WindowBridge(frameSource: nil), files = DragRangeFiles()
        bridge.fileAccess = files
        var failures = 0, release: CheckedContinuation<[URL], Never>?
        let drag = FileDragBridge(bridge: bridge, reportError: { _ in failures += 1 })
        defer { drag.disconnect(); bridge.clipboard.stop() }
        bridge.publishGuestFiles = { _, _, _ in await withCheckedContinuation { release = $0 } }
        try offer(drag); try await until { release != nil }
        let view = DragRecordingView()
        XCTAssertFalse(drag.beginExportIfNeeded(view: view, event: try event()))
        drag.pointerReleased()
        try XCTUnwrap(release).resume(returning: [URL(fileURLWithPath: "/Volumes/Test Shared/folder", isDirectory: true)])
        try await Task.sleep(for: .milliseconds(20))
        XCTAssertFalse(drag.beginExportIfNeeded(view: view, event: try event()))
        XCTAssertTrue(view.draggedItems.isEmpty); XCTAssertEqual(files.exports, 0); XCTAssertEqual(failures, 0)
    }

    func testDisconnectWhileMountWaitsCannotStartLaterDrag() async throws {
        let bridge = WindowBridge(frameSource: nil), files = DragRangeFiles()
        bridge.fileAccess = files
        var failures = 0, release: CheckedContinuation<[URL], Never>?
        let drag = FileDragBridge(bridge: bridge, reportError: { _ in failures += 1 })
        defer { drag.disconnect(); bridge.clipboard.stop() }
        bridge.publishGuestFiles = { _, _, _ in await withCheckedContinuation { release = $0 } }
        try offer(drag); try await until { release != nil }
        let view = DragRecordingView()
        XCTAssertFalse(drag.beginExportIfNeeded(view: view, event: try event()))
        drag.disconnect()
        try XCTUnwrap(release).resume(returning: [URL(fileURLWithPath: "/Volumes/Test Shared/folder", isDirectory: true)])
        try await Task.sleep(for: .milliseconds(20))
        XCTAssertFalse(drag.beginExportIfNeeded(view: view, event: try event()))
        XCTAssertTrue(view.draggedItems.isEmpty); XCTAssertEqual(files.exports, 0); XCTAssertEqual(failures, 0)
    }

    func testUnavailableVolumeDragsPromisesThatCopyOverFileConnection() async throws {
        _ = NSApplication.shared
        for publisher: GuestFilePublisher? in [{ _, _, _ in throw GuestFileSharingError.unavailable }, nil] {
            let bridge = WindowBridge(frameSource: nil), files = DragRangeFiles()
            bridge.fileAccess = files
            var failures = 0
            let drag = FileDragBridge(bridge: bridge, reportError: { _ in failures += 1 })
            defer { drag.disconnect(); bridge.clipboard.stop() }
            bridge.publishGuestFiles = publisher
            try offer(drag)
            // Let publication fail first: a pending gesture resumed without a
            // pressed button would be discarded instead.
            try await Task.sleep(for: .milliseconds(20))
            let view = DragRecordingView()
            XCTAssertTrue(drag.beginExportIfNeeded(view: view, event: try event()))
            let promise = try XCTUnwrap(view.draggedItems.first?.item as? NSFilePromiseProvider)
            XCTAssertEqual(promise.delegate?.filePromiseProvider(promise, fileNameForType: promise.fileType), "folder.txt")
            // Nothing is copied until the destination asks for the promise.
            XCTAssertEqual(files.exports, 0); XCTAssertEqual(failures, 0)
        }
    }
}
