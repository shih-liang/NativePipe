import XCTest
@testable import NativePipeRemoteApp

final class NativePipeRemoteAppTests: XCTestCase {
    func testCommandStartsAfterDestination() throws {
        let options = try RemotePipeCLI.parse(["-p", "2222", "--install-compositor", "me@host",
                                               "firefox", "--private-window", "a b"])
        XCTAssertEqual(options.command?.sshArguments, ["-p", "2222"])
        XCTAssertEqual(options.command?.application, ["firefox", "--private-window", "a b"])
        XCTAssertEqual(options.command?.installCompositor, true)
    }
    func testRequiresApplicationAndRejectsOldForwardingSyntax() {
        XCTAssertThrowsError(try RemotePipeCLI.parse(["me@host"]))
        XCTAssertThrowsError(try RemotePipeCLI.parse(["-L", "1025:localhost:1025", "me@host", "true"]))
        XCTAssertThrowsError(try RemotePipeCLI.parse(["--host", "localhost"]))
    }
    func testHelpBeforeDestinationIsLocalAndAfterDestinationIsPassedThrough() throws {
        XCTAssertTrue(try RemotePipeCLI.parse(["--help"]).wantHelp)
        let command = try RemotePipeCLI.parse(["me@host", "firefox", "--help"])
        XCTAssertFalse(command.wantHelp)
        XCTAssertEqual(command.command?.application, ["firefox", "--help"])
    }
}
