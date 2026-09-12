import XCTest
@testable import NativePipeRemote

final class RemoteInstallerTests: XCTestCase {
    func testVMRuntimeAndPrereleasesDoNotHideCompositorRelease() throws {
        typealias Release = RemoteCompositorRelease.Release
        func release(_ tag: String, name: String, prerelease: Bool = false) -> Release {
            .init(draft: false, prerelease: prerelease, assets: [
                .init(name: name, browser_download_url: URL(string: "https://github.com/shih-liang/NativePipe/releases/download/\(tag)/\(name)")!),
                .init(name: "SHA256SUMS", browser_download_url: URL(string: "https://github.com/shih-liang/NativePipe/releases/download/\(tag)/SHA256SUMS")!)
            ])
        }
        let name = "nativepipe-compositor-x86_64-gnu.tar.gz"
        let runtime = release("runtime-v9", name: "nativepipe-runtime-x86_64.tar")
        XCTAssertNil(try RemoteCompositorRelease.select([runtime]))
        XCTAssertEqual(try RemoteCompositorRelease.select([
            runtime, release("v3-beta", name: name, prerelease: true), release("v2", name: name), release("v1", name: name)
        ])?.absoluteString, "https://github.com/shih-liang/NativePipe/releases/download/v2/")
        XCTAssertThrowsError(try RemoteCompositorRelease.select([
            .init(draft: false, prerelease: false, assets: [
                .init(name: name, browser_download_url: URL(string: "https://example.com/\(name)")!),
                .init(name: "SHA256SUMS", browser_download_url: URL(string: "https://example.com/SHA256SUMS")!)
            ])
        ]))
    }

    func testInstallUpdateCacheIntegrityAndConcurrentPublication() throws {
        try runInstallerFixture(upload: false)
    }

    func testBundledUploadAtomicInstallCacheAndProtocolBoundary() throws {
        try runInstallerFixture(upload: true)
    }

    private func runInstallerFixture(upload: Bool) throws {
        let fixture = try XCTUnwrap(Bundle.module.url(forResource: "installer-check", withExtension: "py", subdirectory: "Resources"))
        let process = Process(), input = Pipe(), output = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        process.arguments = [fixture.path] + (upload ? ["--upload"] : [])
        process.standardInput = input
        process.standardOutput = output
        process.standardError = output
        try process.run()
        let script = SSHCommand(destination: "test", application: ["true"], installCompositor: true)
            .makeRemoteScript(uploadCompositor: upload)
        try input.fileHandleForWriting.write(contentsOf: Data(script.utf8))
        try input.fileHandleForWriting.close()
        let result = output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        XCTAssertEqual(process.terminationStatus, 0, String(decoding: result, as: UTF8.self))
    }
}
