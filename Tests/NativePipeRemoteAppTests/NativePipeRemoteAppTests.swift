import XCTest
import NativePipeStrings
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
    func testVersionIsLocalBeforeDestinationAndPassedThroughAfterDestination() throws {
        let local = try RemotePipeCLI.parse(["--version"])
        XCTAssertTrue(local.wantVersion)
        XCTAssertNil(local.command)
        XCTAssertEqual(NativePipeVersion.current, "0.2.0")
        let remote = try RemotePipeCLI.parse(["me@host", "firefox", "--version"])
        XCTAssertFalse(remote.wantVersion)
        XCTAssertEqual(remote.command?.application, ["firefox", "--version"])
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

    func testSafeSSHSwitchesPassThroughInOrder() throws {
        let value = try RemotePipeCLI.parse(["-4", "-A", "-p", "2222", "-a", "-6", "me@host", "app", "-A"])
        XCTAssertEqual(value.command?.sshArguments, ["-4", "-A", "-p", "2222", "-a", "-6"])
        // After the destination, -A is the Linux application's argument.
        XCTAssertEqual(value.command?.application, ["app", "-A"])
    }
    /// SSH's stdio is the binary transport. -t would override the built-in -T
    /// and push the frames through a terminal; -N and -f leave no remote
    /// command; -W replaces it. -l is excluded because saved credentials are
    /// keyed by the destination, which -l would make ambiguous.
    func testSwitchesThatWouldBreakTheTransportOrIdentityAreRejected() {
        for args in [["-t", "me@host", "app"], ["-tt", "me@host", "app"],
                     ["-N", "me@host", "app"], ["-f", "me@host", "app"],
                     ["-W", "host:22", "me@host", "app"], ["-l", "me", "host", "app"],
                     ["-4A", "me@host", "app"]] {
            XCTAssertThrowsError(try RemotePipeCLI.parse(args), "\(args)")
        }
    }

    // MARK: Failure and exit reporting

    private struct Failure: LocalizedError {
        let errorDescription: String?
        init(_ text: String) { errorDescription = text }
    }

    private func line(connected: Bool, status: Int32?, error: String = "unused") -> String {
        RemotePipeCLI.failureLine(destination: "me@host", application: "gtk4-demo",
                                  connected: connected, remoteExitStatus: status, error: Failure(error))
    }

    /// The last line states the outcome the user can act on; whatever SSH or
    /// the compositor said is already on screen above it and is not repeated.
    func testFailureLineNamesTheOutcomeForEachWayASessionEnds() {
        XCTAssertEqual(line(connected: false, status: 255),
            "Couldn’t connect to me@host. Make sure ssh me@host works with the same options, then try again.")
        XCTAssertEqual(line(connected: false, status: 127),
            "Couldn’t start the session on me@host. The messages above show why.")
        // Once a session is up, 255 is SSH losing the connection, not the app.
        XCTAssertEqual(line(connected: true, status: 255), "Lost the connection to me@host.")
        XCTAssertEqual(line(connected: true, status: 139), "gtk4-demo quit unexpectedly (signal 11).")
        XCTAssertEqual(line(connected: true, status: 3), "gtk4-demo exited with status 3.")
        // A failure this Mac detected is not on screen yet: show its first line.
        XCTAssertEqual(line(connected: true, status: nil, error: "Stream broke.\nlog"),
                       "Disconnected from me@host: Stream broke.")
        XCTAssertEqual(line(connected: false, status: nil, error: "Timed out.\nlog"), "Timed out.")
    }

    /// Ctrl-C already ends the process with SIGINT, which shells report as 130;
    /// a dismissed sign-in means the same thing and must not report success.
    func testCancelledSignInIsNotReportedAsSuccess() {
        XCTAssertEqual(RemotePipeCLI.exitCode(signInCancelled: true, sessionStatus: nil), 130)
        XCTAssertEqual(RemotePipeCLI.exitCode(signInCancelled: false, sessionStatus: nil), 0)
        XCTAssertEqual(RemotePipeCLI.exitCode(signInCancelled: false, sessionStatus: 255), 255)
    }

    func testApplicationNameIsTheProgramWithoutItsDirectory() throws {
        let value = try RemotePipeCLI.parse(["me@host", "/usr/bin/gtk4-demo", "--flag"])
        XCTAssertEqual(RemotePipeCLI.applicationName(try XCTUnwrap(value.command)), "gtk4-demo")
    }

    /// A known ssh option gets advice naming it; anything else gets a message
    /// that repeats nothing the user typed, in case it was a secret.
    func testRejectedSSHOptionsExplainThemselvesWithoutEchoingOtherInput() {
        for (option, hint) in [("-l", "user@host"), ("-t", "terminal"), ("-N", "compositor"),
                               ("-L", "forward ports"), ("-vv", "LogLevel=DEBUG")] {
            XCTAssertThrowsError(try RemotePipeCLI.parse([option, "x", "me@host", "app"])) { error in
                XCTAssertTrue(error.localizedDescription.contains(hint), "\(option): \(error.localizedDescription)")
            }
        }
        XCTAssertThrowsError(try RemotePipeCLI.parse(["--hunter2", "me@host", "app"])) { error in
            XCTAssertFalse(error.localizedDescription.contains("hunter2"))
            XCTAssertTrue(error.localizedDescription.contains("nativepipe --help"))
        }
        XCTAssertThrowsError(try RemotePipeCLI.parse(["-i", "-p", "22", "me@host", "app"])) { error in
            XCTAssertEqual(error.localizedDescription, "Missing value for -i. See nativepipe --help.")
        }
    }
}
