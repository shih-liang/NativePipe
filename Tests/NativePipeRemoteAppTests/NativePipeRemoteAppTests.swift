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

extension NativePipeRemoteAppTests {
    func testInvalidOptionsFailBeforeConnectingAndDoNotEchoPrivateInput() {
        for args in [["-p", "0", "host", "app"], ["-p", "65536", "host", "app"],
                     ["-p", "SECRET", "host", "app"], ["-i", "--help"],
                     ["--compositor", "", "host", "app"], ["--SECRET", "host", "app"]] {
            XCTAssertThrowsError(try RemotePipeCLI.parse(args)) { error in
                XCTAssertFalse(error.localizedDescription.contains("SECRET"))
            }
        }
    }
    func testLocalHelpNeverCarriesAnExecutableCommand() throws {
        for args in [["-h"], ["--help"], ["--install-compositor", "--help"], ["-p", "2222", "-h"]] {
            let value = try RemotePipeCLI.parse(args)
            XCTAssertTrue(value.wantHelp); XCTAssertNil(value.command)
        }
    }
    func testQuietProgressAndBatchModeAreOnlyClientOptions() throws {
        let value = try RemotePipeCLI.parse(["--no-progress", "-o", "BatchMode=yes", "host", "app", "path with spaces", "-h"])
        XCTAssertFalse(value.progress)
        XCTAssertEqual(value.command?.sshArguments, ["-o", "BatchMode=yes"])
        XCTAssertEqual(value.command?.application, ["app", "path with spaces", "-h"])
    }
}
