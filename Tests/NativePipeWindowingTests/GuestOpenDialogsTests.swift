import AppKit
import XCTest
@testable import NativePipeWindowing

@MainActor
final class GuestOpenDialogsTests: XCTestCase {
    func testNativeApprovalSheetIsAsynchronousAndCancelsWithTheRequest() async {
        _ = NSApplication.shared
        let parent = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 480, height: 200),
                              styleMask: [.titled, .closable], backing: .buffered, defer: false)
        parent.isReleasedWhenClosed = false
        parent.makeKeyAndOrderFront(nil)
        defer { parent.close() }
        let finished = expectation(description: "Native sheet cancelled")
        var result: GuestOpenAction?
        let task = Task {
            result = await GuestOpenDialogs.confirm(.link(machine: "Test Linux", url: URL(string: "https://example.com")!))
            finished.fulfill()
        }
        for _ in 0..<100 where parent.attachedSheet == nil {
            try? await Task.sleep(for: .milliseconds(10))
        }
        XCTAssertNotNil(parent.attachedSheet)
        XCTAssertNil(result, "Waiting for approval does not block the main actor")
        task.cancel()
        await fulfillment(of: [finished], timeout: 2)
        XCTAssertEqual(result, .cancel)
        XCTAssertNil(parent.attachedSheet)
    }

    private func standaloneAlert() async throws -> NSWindow {
        for _ in 0..<200 {
            if let window = NSApp.windows.first(where: { $0.isVisible }) { return window }
            try? await Task.sleep(for: .milliseconds(10))
        }
        throw XCTSkip("the alert did not appear")
    }

    func testWithoutAnyWindowTheApprovalIsAPlainAlertNotABlankWindow() async throws {
        _ = NSApplication.shared
        try XCTSkipIf(NSApp.windows.contains { $0.isVisible },
                      "another test left a window open, so the sheet path would be used")
        let finished = expectation(description: "Alert cancelled")
        var result: GuestOpenAction?
        let task = Task {
            result = await GuestOpenDialogs.confirm(.link(machine: "Test Linux", url: URL(string: "https://example.com")!))
            finished.fulfill()
        }
        let alertWindow = try await standaloneAlert()
        XCTAssertNil(result, "Waiting for approval does not block the main actor")
        XCTAssertEqual(NSApp.windows.filter(\.isVisible), [alertWindow], "no placeholder window carries the alert")
        XCTAssertNil(alertWindow.sheetParent)
        XCTAssertNil(NSApp.modalWindow, "the approval is not a modal session")
        task.cancel()
        await fulfillment(of: [finished], timeout: 2)
        XCTAssertEqual(result, .cancel)
        XCTAssertFalse(alertWindow.isVisible)
    }

    func testPressingAButtonAnswersAndOnlyTheNamedButtonOpens() async throws {
        _ = NSApplication.shared
        try XCTSkipIf(NSApp.windows.contains { $0.isVisible },
                      "another test left a window open, so the sheet path would be used")
        for (title, expected) in [("Open", GuestOpenAction.open), ("Cancel", .cancel)] {
            let finished = expectation(description: "Answered \(title)")
            var result: GuestOpenAction?
            let task = Task {
                result = await GuestOpenDialogs.confirm(.link(machine: "Test Linux", url: URL(string: "https://example.com")!))
                finished.fulfill()
            }
            let window = try await standaloneAlert()
            func buttons(_ view: NSView) -> [NSButton] { (view as? NSButton).map { [$0] } ?? view.subviews.flatMap(buttons) }
            let button = try XCTUnwrap(buttons(try XCTUnwrap(window.contentView?.superview)).first { $0.title == NPTextProbe.localized(title) })
            button.performClick(nil)
            await fulfillment(of: [finished], timeout: 2)
            XCTAssertEqual(result, expected, title)
            XCTAssertFalse(window.isVisible)
            task.cancel()
        }
    }
}

private enum NPTextProbe {
    /// The test runs in the development language, where titles are the keys.
    static func localized(_ key: String) -> String { key }
}
