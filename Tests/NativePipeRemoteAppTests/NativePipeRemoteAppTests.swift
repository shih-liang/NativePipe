import XCTest
@testable import NativePipeRemoteApp

final class NativePipeRemoteAppTests: XCTestCase {
    func testParsesDestinationAndPassesSSHOptions() throws {
        let cli = try RemotePipeCLI.parse([
            "lfs@example.test", "-p", "2222",
            "--compositor", "/opt/remotepipe-wayland",
        ])
        XCTAssertEqual(cli.destination, "lfs@example.test")
        XCTAssertEqual(cli.sshArguments, ["-p", "2222"])
        XCTAssertEqual(cli.compositor, "/opt/remotepipe-wayland")
    }

    func testLocalPortsAndExplicitSSHSeparator() throws {
        let cli = try RemotePipeCLI.parse([
            "--host", "localhost", "--surface-port", "41025",
            "--media-port=41026", "--", "-o", "ProxyJump=bastion",
        ])
        XCTAssertNil(cli.destination)
        XCTAssertEqual(cli.host, "localhost")
        XCTAssertEqual(cli.surfacePort, 41025)
        XCTAssertEqual(cli.mediaPort, 41026)
        XCTAssertEqual(cli.sshArguments, ["-o", "ProxyJump=bastion"])
    }

    func testRejectsMissingOptionValue() {
        XCTAssertThrowsError(try RemotePipeCLI.parse(["--media-port"]))
    }

    func testEnsureScriptUsesPrivatePerUserRuntimeAndStrictOwnership() {
        let script = SSHBootstrap.ensureScript(compositor: "/opt/remotepipe wayland")
        XCTAssertTrue(script.contains("/tmp/remotepipe-xdg-$(id -u)"))
        XCTAssertTrue(script.contains("chmod 700 \"$RUNTIME\""))
        XCTAssertTrue(script.contains("tracked_alive"))
        XCTAssertTrue(script.contains("ports 1025/1026 belong to an untracked process"))
        XCTAssertTrue(script.contains("tracked compositor is alive but both ports are not ready"))
        XCTAssertTrue(script.contains("'/opt/remotepipe wayland'"))
    }
}
