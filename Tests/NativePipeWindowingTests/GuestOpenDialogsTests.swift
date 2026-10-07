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
}
