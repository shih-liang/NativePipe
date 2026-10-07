import Foundation
import XCTest
import NativePipeStrings
import Darwin
import NativePipeProtocol
@testable import NativePipeRemote

private final class SFTPReadGate: SFTPTransport, @unchecked Sendable {
    private let base: SFTPTransport
    private let condition = NSCondition()
    private var waiting = false, released = false, canceled = false
    init(_ base: SFTPTransport) { self.base = base }
    var readIsWaiting: Bool { condition.lock(); defer { condition.unlock() }; return waiting }
    var wasCanceled: Bool { condition.lock(); defer { condition.unlock() }; return canceled }
    func releaseRead() { condition.lock(); released = true; condition.broadcast(); condition.unlock() }
    func start() throws { try base.start() }
    func write(_ data: Data) throws {
        if data.count > 4, data[4] == 5 {
            condition.lock()
            if !waiting {
                waiting = true
                while !released { condition.wait() }
            }
            condition.unlock()
        }
        try base.write(data)
    }
    func readPacket() throws -> Data { try base.readPacket() }
    func checkCancellation() throws { try base.checkCancellation() }
    func beginPublication() throws { try base.beginPublication() }
    func endPublication() { base.endPublication() }
    func cancel() {
        condition.lock(); canceled = true; released = true; condition.broadcast(); condition.unlock()
        base.cancel()
    }
    func close() { releaseRead(); base.close() }
}

final class SFTPFileSystemTests: XCTestCase {
    private func temporaryDirectory() throws -> URL {
        let result = FileManager.default.temporaryDirectory.appendingPathComponent("nativepipe-sftp-protocol-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: result, withIntermediateDirectories: false)
        return result
    }
    private func canonicalPath(_ url: URL) throws -> String {
        guard let path = Darwin.realpath(url.path, nil) else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { free(path) }; return String(cString: path)
    }
    private func rawNames(_ url: URL) throws -> [[UInt8]] {
        guard let directory = opendir(url.path) else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { closedir(directory) }
        var result: [[UInt8]] = []
        while let entry = readdir(directory) {
            let length = Int(entry.pointee.d_namlen)
            let name = withUnsafePointer(to: &entry.pointee.d_name) { pointer in
                pointer.withMemoryRebound(to: UInt8.self, capacity: Int(MAXNAMLEN) + 1) { Array(UnsafeBufferPointer(start: $0, count: length)) }
            }
            if name == [46] || name == [46, 46] { continue }; result.append(name)
        }
        return result
    }
    @MainActor private func realServer(_ root: URL, reuseConnection: Bool = false,
                                       transportObserver: (@Sendable (SFTPTransport) -> Void)? = nil) throws -> SFTPFileSystem {
        let executable = URL(fileURLWithPath: "/usr/libexec/sftp-server")
        guard FileManager.default.isExecutableFile(atPath: executable.path) else { throw XCTSkip("The system SFTP server is unavailable.") }
        return SFTPFileSystem(reuseConnection: reuseConnection, transportFactory: {
            let authentication = try self.temporaryDirectory()
            let transport = SFTPSSHTransport(executable: executable, arguments: ["-d", root.path],
                environment: ProcessInfo.processInfo.environment, authenticationDirectory: authentication)
            transportObserver?(transport); return transport
        })
    }
    /// OpenSSH's server answers OPENDIR on a regular file with "No such file" --
    /// it maps ENOTDIR that way -- so a browser would claim an existing file is
    /// missing. Listing skips a separate LSTAT on
    /// purpose, so the explanation has to come from classifying the refusal.
    @MainActor func testBrowsingAFileExplainsThatADirectoryIsNeeded() async throws {
        let root = try temporaryDirectory(); defer { try? FileManager.default.removeItem(at: root) }
        try Data("contents".utf8).write(to: root.appendingPathComponent("file"))
        let system = try realServer(root)
        let home = try await system.list()
        do {
            _ = try await system.list(path: home.path + "/file")
            XCTFail("A regular file cannot be listed")
        } catch {
            XCTAssertEqual(error.localizedDescription, NPText("Choose a remote directory to browse."))
        }
        // The directory itself still lists normally.
        let listing = try await system.list(path: home.path)
        XCTAssertEqual(listing.entries.map(\.name), ["file"])
    }

    @MainActor func testBrowserReusesOneRealConnectionAndExplicitCloseReleasesIt() async throws {
        let root = try temporaryDirectory(); defer { try? FileManager.default.removeItem(at: root) }
        try FileManager.default.createDirectory(at: root.appendingPathComponent("child"), withIntermediateDirectories: false)
        try Data("contents".utf8).write(to: root.appendingPathComponent("child/file"))
        let transports = SFTPTransportRecorder()
        let system = try realServer(root, reuseConnection: true, transportObserver: { transports.append($0) })
        let home = try await system.list()
        let child = try await system.list(path: home.path + "/child")
        XCTAssertEqual(child.home, home.home)
        XCTAssertEqual(child.entries.first?.size, 8)
        _ = try await system.list(path: home.path)
        XCTAssertEqual(transports.count, 1, "Navigation must not launch SSH and authenticate again")
        system.close()
        _ = try await system.list()
        XCTAssertEqual(transports.count, 2)
        system.close()
    }
    @MainActor func testLazyRangesUseRealSFTPReadsWithoutDownloadingLargePrefixes() async throws {
        let root = try temporaryDirectory(); defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("sparse")
        let descriptor = open(source.path, O_CREAT | O_EXCL | O_RDWR, 0o751)
        XCTAssertGreaterThanOrEqual(descriptor, 0); defer { close(descriptor) }
        let offset: UInt64 = 1 << 40, bytes = Array("selected range".utf8)
        XCTAssertEqual(bytes.withUnsafeBytes { pwrite(descriptor, $0.baseAddress, $0.count, off_t(offset)) }, bytes.count)
        let canonical = try canonicalPath(source)
        let system = try realServer(root, reuseConnection: true)
        let info = try await system.snapshot(path: canonical)
        XCTAssertEqual(info.size, offset + UInt64(bytes.count)); XCTAssertEqual(info.permissions, 0o751)
        XCTAssertEqual(info.version.count, 32, "The opaque revision must not disclose the remote source path")
        let range = try await system.readRange(path: canonical, offset: offset + 2, length: 5, expectedVersion: info.version)
        XCTAssertEqual(range, Data("lecte".utf8))
        let eof = try await system.readRange(path: canonical, offset: info.size, length: 100, expectedVersion: info.version)
        XCTAssertTrue(eof.isEmpty)
        let empty = try await system.readRange(path: canonical, offset: offset, length: 0, expectedVersion: info.version)
        XCTAssertTrue(empty.isEmpty)
        let listing = try await system.rangeContents(path: (canonical as NSString).deletingLastPathComponent)
        XCTAssertEqual(listing.map(\.url.lastPathComponent), ["sparse"])
        let pipeline = Data((0..<(UserFileRange.maximumReadLength + 64)).map { UInt8(truncatingIfNeeded: $0 * 37) })
        let pipelineURL = root.appendingPathComponent("pipeline")
        try pipeline.write(to: pipelineURL)
        let pipelinePath = try canonicalPath(pipelineURL), pipelineInfo = try await system.snapshot(path: pipelinePath)
        let window = try await system.readRange(path: pipelinePath, offset: 23, length: UserFileRange.maximumReadLength,
                                               expectedVersion: pipelineInfo.version)
        XCTAssertEqual(window, pipeline.subdata(in: 23..<(23 + UserFileRange.maximumReadLength)))
        XCTAssertEqual(ftruncate(descriptor, off_t(info.size + 1)), 0)
        do { _ = try await system.readRange(path: canonical, offset: offset, length: 1, expectedVersion: info.version); XCTFail("Changed source accepted") }
        catch SFTPFileSystemError.sourceChanged { }
        system.close()
    }
    @MainActor func testUserRangeAdapterQueuesOverlappingReadsOnOnePersistentSession() async throws {
        let root = try temporaryDirectory(); defer { try? FileManager.default.removeItem(at: root) }
        try Data("0123456789".utf8).write(to: root.appendingPathComponent("file"))
        let recorder = SFTPTransportRecorder()
        let system = try realServer(root, reuseConnection: true, transportObserver: { recorder.append($0) })
        let access: any UserFileRangeAccess = RemoteUserFileAccess(makeSession: { system })
        let remote = URL(fileURLWithPath: try canonicalPath(root.appendingPathComponent("file")))
        let info = try await access.metadata(for: remote)
        async let first = access.read(remote, offset: 0, length: 4, expectedVersion: info.version)
        async let second = access.read(remote, offset: 6, length: 4, expectedVersion: info.version)
        async let stat = access.metadata(for: remote)
        let (a, b, current) = try await (first, second, stat)
        XCTAssertEqual(a, Data("0123".utf8)); XCTAssertEqual(b, Data("6789".utf8))
        XCTAssertEqual(current.version, info.version)
        XCTAssertEqual(recorder.count, 1, "FSKit reads must not reconnect SSH per requested range")
        access.closeRangeAccess()
        _ = try await access.metadata(for: remote)
        XCTAssertEqual(recorder.count, 2)
        access.closeRangeAccess()
    }
    @MainActor func testCancelingQueuedRangeReturnsPromptlyWithoutCancelingItsPredecessor() async throws {
        let root = try temporaryDirectory(), authentication = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: authentication) }
        let executable = URL(fileURLWithPath: "/usr/libexec/sftp-server")
        guard FileManager.default.isExecutableFile(atPath: executable.path) else { throw XCTSkip("The system SFTP server is unavailable.") }
        try Data("0123456789".utf8).write(to: root.appendingPathComponent("file"))
        let gate = SFTPReadGate(SFTPSSHTransport(executable: executable, arguments: ["-d", root.path],
            environment: ProcessInfo.processInfo.environment, authenticationDirectory: authentication))
        let watchdog = DispatchWorkItem { gate.releaseRead() }
        DispatchQueue.global().asyncAfter(deadline: .now() + 2, execute: watchdog)
        defer { watchdog.cancel(); gate.releaseRead() }
        let system = SFTPFileSystem(reuseConnection: true, transportFactory: { gate })
        let access = RemoteUserFileAccess(makeSession: { system })
        let remote = URL(fileURLWithPath: try canonicalPath(root.appendingPathComponent("file")))
        let info = try await access.metadata(for: remote)
        let first = Task { try await access.read(remote, offset: 0, length: 4, expectedVersion: info.version) }
        for _ in 0..<100 {
            if gate.readIsWaiting { break }
            try await Task.sleep(for: .milliseconds(10))
        }
        XCTAssertTrue(gate.readIsWaiting)
        let queued = Task { try await access.read(remote, offset: 6, length: 4, expectedVersion: info.version) }
        try await Task.sleep(for: .milliseconds(20))
        queued.cancel()
        let started = ProcessInfo.processInfo.systemUptime
        do { _ = try await queued.value; XCTFail("Canceled queued read succeeded") }
        catch is CancellationError { }
        XCTAssertLessThan(ProcessInfo.processInfo.systemUptime - started, 0.5)
        XCTAssertFalse(gate.wasCanceled, "Canceling a queued request must preserve the earlier handle read")
        gate.releaseRead()
        let bytes = try await first.value
        XCTAssertEqual(bytes, Data("0123".utf8))
        access.closeRangeAccess()
    }
    @MainActor func testLazySFTPPathsRejectFinalAndAncestorSymlinks() async throws {
        let root = try temporaryDirectory(); defer { try? FileManager.default.removeItem(at: root) }
        try FileManager.default.createDirectory(at: root.appendingPathComponent("folder"), withIntermediateDirectories: false)
        try Data("contents".utf8).write(to: root.appendingPathComponent("folder/file"))
        try FileManager.default.createSymbolicLink(atPath: root.appendingPathComponent("folder/link").path, withDestinationPath: "file")
        try FileManager.default.createSymbolicLink(atPath: root.appendingPathComponent("directory-link").path, withDestinationPath: "folder")
        let system = try realServer(root, reuseConnection: true), canonical = try canonicalPath(root)
        let listing = try await system.rangeContents(path: canonical + "/folder")
        XCTAssertEqual(listing.map(\.url.lastPathComponent), ["file"])
        for suffix in ["/folder/link", "/directory-link/file"] {
            do { _ = try await system.snapshot(path: canonical + suffix); XCTFail("Lazy access followed symlink") }
            catch SFTPFileSystemError.unsupportedFile { }
        }
        do { _ = try await system.rangeContents(path: canonical + "/directory-link"); XCTFail("Lazy listing followed symlink") }
        catch SFTPFileSystemError.unsupportedFile { }
        system.close()
    }
    @MainActor func testLazySFTPMetadataKeepsByteDistinctNFCAndNFDFilenames() async throws {
        let names = ["\u{e9}.txt", "e\u{301}.txt"]
        let transport = SFTPListingFixture(names: names, includeTimes: true, preserveCanonicalPaths: true)
        let system = SFTPFileSystem(reuseConnection: true, transportFactory: { transport })
        let entries = try await system.rangeContents(path: "/fixture")
        XCTAssertEqual(Set(entries.map { Data($0.url.path.utf8) }), Set(names.map { Data(("/fixture/" + $0).utf8) }))
        XCTAssertEqual(Set(entries.map(\.version)).count, 2)
        XCTAssertTrue(entries.allSatisfy { $0.version.count == 32 })
        let snapshot = try await system.snapshot(path: "/fixture/\u{e9}.txt")
        XCTAssertEqual(Data(snapshot.url.path.utf8), Data("/fixture/\u{e9}.txt".utf8))
        system.close()
    }
    @MainActor func testIsolatedOperationsStillUseSeparateRealConnections() async throws {
        let root = try temporaryDirectory(); defer { try? FileManager.default.removeItem(at: root) }
        let transports = SFTPTransportRecorder()
        let system = try realServer(root, transportObserver: { transports.append($0) })
        _ = try await system.list(); _ = try await system.list()
        XCTAssertEqual(transports.count, 2)
    }
    @MainActor func testReusableSessionCachesHomeAndDiscardsFailedProtocolStream() async throws {
        let counter = SFTPListingSessionRecorder()
        let system = SFTPFileSystem(reuseConnection: true, transportFactory: { counter.makeTransport() })
        _ = try await system.list()
        _ = try await system.list(path: "/fixture/child")
        XCTAssertEqual(counter.count, 1)
        XCTAssertEqual(counter.first.realpathCount, 2, "Home is queried once; navigation only canonicalizes its target")
        XCTAssertEqual(counter.first.lstatCount, 0, "OPENDIR itself validates the directory")
        counter.first.failNextRequest()
        do { _ = try await system.list(path: "/fixture/broken"); XCTFail("A failed response must not be reused") }
        catch SFTPFailure.protocolError { }
        XCTAssertTrue(counter.first.closed)
        _ = try await system.list()
        XCTAssertEqual(counter.count, 2)
        system.close()
        XCTAssertTrue(counter.last.closed)
    }
    @MainActor func testDirectoryCloseRefusalOrTimeoutDiscardsReusableStream() async throws {
        for mode in [SFTPListingFixture.CloseFailure.refused, .timeout] {
            let counter = SFTPListingSessionRecorder(firstCloseFailure: mode)
            let system = SFTPFileSystem(reuseConnection: true, transportFactory: { counter.makeTransport() })
            do {
                _ = try await system.list()
                XCTFail("A directory must not succeed when its remote handle did not close.")
            } catch {
                switch mode {
                case .refused:
                    guard let failure = error as? SFTPFailure, case .status(3, _) = failure else {
                        return XCTFail("Lost CLOSE refusal: \(error)")
                    }
                case .timeout:
                    XCTAssertEqual(error.localizedDescription, "Injected CLOSE timeout.")
                }
            }
            XCTAssertTrue(counter.first.closed, "Unacknowledged CLOSE must release the failed stream.")
            XCTAssertEqual(counter.first.closeRequests, 1, "A failed CLOSE must not be retried by cleanup.")
            let recovered = try await system.list()
            XCTAssertEqual(recovered.entries.map(\.name), ["entry"])
            XCTAssertEqual(counter.count, 2, "The next directory request must start a new protocol session.")
            system.close()
            XCTAssertTrue(counter.last.closed)
        }
    }
    @MainActor func testDirectoryReadErrorSurvivesBestEffortCloseFailure() async throws {
        for mode in [SFTPListingFixture.CloseFailure.refused, .timeout] {
            let transport = SFTPListingFixture(names: ["../escape"], closeFailure: mode)
            let system = SFTPFileSystem(reuseConnection: true, transportFactory: { transport })
            do { _ = try await system.list(); XCTFail("The invalid directory reply must fail.") }
            catch SFTPFailure.protocolError { }
            XCTAssertEqual(transport.closeRequests, 1)
            XCTAssertTrue(transport.closed, "Failed listing cleanup must release the retained stream.")
        }
    }
    @MainActor func testFailedDirectoryClassificationPreservesOriginalStatus() async throws {
        let transport = SFTPListingFixture(names: ["entry"], closeFailure: .refused, failStat: true)
        let system = SFTPFileSystem(reuseConnection: true, transportFactory: { transport })
        do { _ = try await system.list(); XCTFail("The failed directory operation must remain a failure.") }
        catch {
            guard let failure = error as? SFTPFailure, case .status(3, _) = failure else {
                return XCTFail("The failed classification replaced the original status: \(error)")
            }
        }
        XCTAssertEqual(transport.lstatCount, 1)
        XCTAssertTrue(transport.closed)
    }
    @MainActor func testRealServerCanonicalHomeLiteralNamesAndSymlinkMetadata() async throws {
        let root = try temporaryDirectory(); defer { try? FileManager.default.removeItem(at: root) }
        let name = "space \\\" *[1]\n.txt"
        try Data("literal".utf8).write(to: root.appendingPathComponent(name))
        try FileManager.default.createDirectory(at: root.appendingPathComponent("folder"), withIntermediateDirectories: false)
        try FileManager.default.createSymbolicLink(atPath: root.appendingPathComponent("link").path, withDestinationPath: "folder")
        let system = try realServer(root)
        let directory = try await system.list()
        XCTAssertEqual(directory.home, try canonicalPath(root))
        XCTAssertEqual(directory.path, directory.home)
        let literal = try XCTUnwrap(directory.entries.first { $0.name == name })
        XCTAssertEqual(literal.kind, .file); XCTAssertEqual(literal.size, 7); XCTAssertNotNil(literal.modified)
        XCTAssertEqual(directory.entries.first { $0.name == "link" }?.kind, .symbolicLink)
        XCTAssertEqual(directory.entries.first { $0.name == "folder" }?.kind, .directory)
        let stat = try await system.stat(path: literal.path)
        XCTAssertEqual(stat, literal)
    }
    @MainActor func testRealServerMkdirAndExistingDestinationAreExplicit() async throws {
        let root = try temporaryDirectory(); defer { try? FileManager.default.removeItem(at: root) }
        let system = try realServer(root)
        try await system.createDirectory(path: "new *[folder]")
        XCTAssertTrue(FileManager.default.fileExists(atPath: root.appendingPathComponent("new *[folder]").path))
        do { try await system.createDirectory(path: "new *[folder]"); XCTFail("mkdir must not silently reuse a folder") }
        catch SFTPFileSystemError.destinationExists { }
        do { try await system.createDirectory(path: "../.. /bad"); XCTFail("A missing parent must fail") } catch { }
    }
    @MainActor func testRealServerRecursiveRoundTripWithBoundedPipeliningAndAcknowledgedBytes() async throws {
        let root = try temporaryDirectory(), local = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: local) }
        let source = local.appendingPathComponent("source"), destination = local.appendingPathComponent("received")
        try FileManager.default.createDirectory(at: source.appendingPathComponent("nested/empty"), withIntermediateDirectories: true)
        let bytes = Data((0..<(2_097_152 + 17)).map { UInt8(truncatingIfNeeded: $0) })
        try bytes.write(to: source.appendingPathComponent("nested/large *[1].bin"))
        try Data().write(to: source.appendingPathComponent("zero"))
        let timestamp = Date(timeIntervalSince1970: 1_700_000_000)
        try FileManager.default.setAttributes([.posixPermissions: 0o751, .modificationDate: timestamp],
            ofItemAtPath: source.appendingPathComponent("nested/large *[1].bin").path)
        try FileManager.default.setAttributes([.posixPermissions: 0o750, .modificationDate: timestamp],
            ofItemAtPath: source.appendingPathComponent("nested").path)
        let system = try realServer(root), upload = SFTPProgressRecorder(), download = SFTPProgressRecorder()
        try await system.transfer(direction: .upload, local: source, remote: "exact destination", progress: { [upload] sample in upload.append(sample) })
        XCTAssertEqual(try Data(contentsOf: root.appendingPathComponent("exact destination/nested/large *[1].bin")), bytes)
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent("exact destination/source").path))
        try await system.transfer(direction: .download, local: destination, remote: "exact destination", progress: { [download] sample in download.append(sample) })
        XCTAssertEqual(try Data(contentsOf: destination.appendingPathComponent("nested/large *[1].bin")), bytes)
        XCTAssertTrue(FileManager.default.fileExists(atPath: destination.appendingPathComponent("nested/empty").path))
        for base in [root.appendingPathComponent("exact destination"), destination] {
            let file = try FileManager.default.attributesOfItem(atPath: base.appendingPathComponent("nested/large *[1].bin").path)
            let folder = try FileManager.default.attributesOfItem(atPath: base.appendingPathComponent("nested").path)
            XCTAssertEqual((file[.posixPermissions] as? NSNumber)?.intValue, 0o751)
            XCTAssertEqual((folder[.posixPermissions] as? NSNumber)?.intValue, 0o750)
            XCTAssertEqual(file[.modificationDate] as? Date, timestamp)
            XCTAssertEqual(folder[.modificationDate] as? Date, timestamp)
        }
        for recorder in [upload, download] {
            let samples = recorder.samples
            XCTAssertEqual(samples.first?.bytesTransferred, 0)
            XCTAssertEqual(samples.first?.totalBytes, UInt64(bytes.count))
            XCTAssertEqual(samples.last?.bytesTransferred, UInt64(bytes.count))
            XCTAssertEqual(samples.last?.isComplete, true)
            XCTAssertTrue(samples.dropLast().allSatisfy { !$0.isComplete })
            XCTAssertEqual(samples.map(\.bytesTransferred), samples.map(\.bytesTransferred).sorted())
            XCTAssertEqual(samples.first { $0.bytesTransferred > 0 }?.bytesTransferred, 32_768)
            XCTAssertTrue(samples.count >= 3 && samples.count < 10, "Progress callbacks should be throttled while byte accounting stays exact")
        }
        XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: root.path).contains { $0.hasPrefix(".nativepipe-") })
    }
    @MainActor func testRealServerPreservesNFCFilenameBytesAcrossRecursiveRoundTrip() async throws {
        let root = try temporaryDirectory(), local = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: local) }
        let source = local.appendingPathComponent("source"), destination = local.appendingPathComponent("received")
        try FileManager.default.createDirectory(at: source, withIntermediateDirectories: false)
        let name = "計畫 Résumé.txt", folderName = "café folder"
        let folderPath = source.path + "/" + folderName
        guard Darwin.mkdir(folderPath, 0o750) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        let fd = Darwin.open(folderPath + "/" + name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0o640)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        let bytes = Data("NFC filename byte identity\n".utf8)
        let written = bytes.withUnsafeBytes { Darwin.write(fd, $0.baseAddress!, bytes.count) }
        Darwin.close(fd)
        XCTAssertEqual(written, bytes.count)
        let expectedFolderBytes = Array(folderName.utf8), expectedNameBytes = Array(name.utf8)
        XCTAssertTrue(try rawNames(source).contains(expectedFolderBytes), "Fixture must actually create an NFC folder through POSIX")
        let sourceFolderURL = URL(fileURLWithPath: folderPath)
        XCTAssertTrue(try rawNames(sourceFolderURL).contains(expectedNameBytes), "Fixture must actually create an NFC file through POSIX")
        let system = try realServer(root)
        try await system.transfer(direction: .upload, local: source, remote: "uploaded", progress: { _ in })
        XCTAssertTrue(try rawNames(root.appendingPathComponent("uploaded")).contains(expectedFolderBytes))
        XCTAssertTrue(try rawNames(root.appendingPathComponent("uploaded").appendingPathComponent(folderName)).contains(expectedNameBytes))
        try await system.transfer(direction: .download, local: destination, remote: "uploaded", progress: { _ in })
        XCTAssertTrue(try rawNames(destination).contains(expectedFolderBytes), "Download must retain NFC folder bytes")
        XCTAssertTrue(try rawNames(destination.appendingPathComponent(folderName)).contains(expectedNameBytes), "Download must retain NFC file bytes")
        XCTAssertEqual(try Data(contentsOf: destination.appendingPathComponent(folderName).appendingPathComponent(name)), bytes)
    }
    @MainActor func testRealServerRefusesExistingTargetsAndSupportsExplicitTreeReplacement() async throws {
        let root = try temporaryDirectory(), local = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: local) }
        let source = local.appendingPathComponent("source"), remote = root.appendingPathComponent("target"), received = local.appendingPathComponent("received")
        try Data("new".utf8).write(to: source); try Data("old".utf8).write(to: remote); try Data("local old".utf8).write(to: received)
        let system = try realServer(root)
        do { try await system.transfer(direction: .upload, local: source, remote: "target", progress: { _ in }); XCTFail("default must not overwrite") }
        catch SFTPFileSystemError.destinationExists { }
        XCTAssertEqual(try String(contentsOf: remote, encoding: .utf8), "old")
        do { try await system.transfer(direction: .download, local: received, remote: "target", progress: { _ in }); XCTFail("default must not overwrite") }
        catch SFTPFileSystemError.destinationExists { }
        XCTAssertEqual(try String(contentsOf: received, encoding: .utf8), "local old")
        try await system.transfer(direction: .upload, local: source, remote: "target", overwrite: true, progress: { _ in })
        try await system.transfer(direction: .download, local: received, remote: "target", overwrite: true, progress: { _ in })
        XCTAssertEqual(try String(contentsOf: received, encoding: .utf8), "new")
        let folder = local.appendingPathComponent("tree")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)
        try Data("first".utf8).write(to: folder.appendingPathComponent("one"))
        try await system.transfer(direction: .upload, local: folder, remote: "tree", progress: { _ in })
        try FileManager.default.removeItem(at: folder.appendingPathComponent("one"))
        try Data("second".utf8).write(to: folder.appendingPathComponent("two"))
        try await system.transfer(direction: .upload, local: folder, remote: "tree", overwrite: true, progress: { _ in })
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent("tree/one").path))
        XCTAssertEqual(try String(contentsOf: root.appendingPathComponent("tree/two"), encoding: .utf8), "second")
        XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: root.path).contains { $0.hasPrefix(".nativepipe-") })
    }
    @MainActor func testRealServerRejectsSymlinkTreesBeforePublishing() async throws {
        let root = try temporaryDirectory(), local = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: local) }
        let source = local.appendingPathComponent("source")
        try FileManager.default.createDirectory(at: source, withIntermediateDirectories: false)
        try FileManager.default.createSymbolicLink(atPath: source.appendingPathComponent("loop").path, withDestinationPath: source.path)
        let system = try realServer(root)
        do { try await system.transfer(direction: .upload, local: source, remote: "target", progress: { _ in }); XCTFail("symlinks must not be traversed") }
        catch SFTPFileSystemError.unsupportedFile { }
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent("target").path))
        try FileManager.default.createSymbolicLink(atPath: root.appendingPathComponent("link").path, withDestinationPath: source.path)
        do { try await system.transfer(direction: .download, local: local.appendingPathComponent("received"), remote: "link", progress: { _ in }); XCTFail("symlinks must not be followed") }
        catch SFTPFileSystemError.unsupportedFile { }
        XCTAssertFalse(FileManager.default.fileExists(atPath: local.appendingPathComponent("received").path))
    }
    @MainActor func testCancelledUploadPreservesExistingDestinationAndNeverReportsCompletion() async throws {
        let root = try temporaryDirectory(), local = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: local) }
        let source = local.appendingPathComponent("source"), target = root.appendingPathComponent("target")
        try Data(repeating: 7, count: 4_194_304).write(to: source); try Data("original".utf8).write(to: target)
        let holder = SFTPTransportHolder(), recorder = SFTPProgressRecorder()
        let system = try realServer(root, transportObserver: { [holder] transport in holder.set(transport) })
        do {
            try await system.transfer(direction: .upload, local: source, remote: "target", overwrite: true, progress: { sample in
                recorder.append(sample)
                if sample.bytesTransferred >= 32_768 { holder.cancel() }
            })
            XCTFail("Cancellation must throw")
        } catch is CancellationError { }
        XCTAssertEqual(try String(contentsOf: target, encoding: .utf8), "original")
        XCTAssertTrue(recorder.samples.allSatisfy { !$0.isComplete })
        XCTAssertLessThan(recorder.samples.last?.bytesTransferred ?? .max, 4_194_304)
        // A new operation owns a new transport and can immediately retry.
        let listing = try await system.list()
        XCTAssertEqual(listing.entries.first { $0.name == "target" }?.size, 8)
        XCTAssertFalse(listing.entries.contains { $0.name.hasPrefix(".nativepipe-transfer-") },
            "Cancellation must clean the unpublished upload using a fresh connection")
    }
    @MainActor func testTaskCancelledFolderUploadCleansStagingAndPreservesOriginalTree() async throws {
        let root = try temporaryDirectory(), local = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: local) }
        let source = local.appendingPathComponent("source"), target = root.appendingPathComponent("target")
        try FileManager.default.createDirectory(at: source.appendingPathComponent("nested"), withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: target, withIntermediateDirectories: false)
        try Data(repeating: 7, count: 4_194_304).write(to: source.appendingPathComponent("nested/large"))
        try Data("original".utf8).write(to: target.appendingPathComponent("old"))
        let holder = SFTPTaskHolder(), recorder = SFTPProgressRecorder(), system = try realServer(root)
        let operation = Task {
            try await system.transfer(direction: .upload, local: source, remote: "target", overwrite: true, progress: { sample in
                recorder.append(sample)
                if sample.bytesTransferred >= 32_768 { holder.cancel() }
            })
        }
        holder.set(operation)
        do { try await operation.value; XCTFail("Cancellation must throw") } catch is CancellationError { }
        XCTAssertTrue(operation.isCancelled)
        XCTAssertEqual(try String(contentsOf: target.appendingPathComponent("old"), encoding: .utf8), "original")
        XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: root.path).contains { $0.hasPrefix(".nativepipe-transfer-") })
        XCTAssertTrue(recorder.samples.allSatisfy { !$0.isComplete })
    }
    @MainActor func testFailedCancellationCleanupReportsExactStagingAndPreservesOriginal() async throws {
        try await exerciseFailedCleanup(stalled: false)
    }
    @MainActor func testCancellationCleanupHasOneOverallDeadlineAndClosesStalledAttempt() async throws {
        try await exerciseFailedCleanup(stalled: true)
    }
    @MainActor private func exerciseFailedCleanup(stalled: Bool) async throws {
        guard FileManager.default.isExecutableFile(atPath: "/usr/libexec/sftp-server") else { throw XCTSkip("The system SFTP server is unavailable.") }
        let root = try temporaryDirectory(), local = try temporaryDirectory(), cleanupAuth = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: local); try? FileManager.default.removeItem(at: cleanupAuth) }
        let source = local.appendingPathComponent("source"), target = root.appendingPathComponent("target")
        try Data(repeating: 7, count: 4_194_304).write(to: source); try Data("original".utf8).write(to: target)
        let attempts = SFTPAttemptCounter(), holder = SFTPTransportHolder(), recorder = SFTPProgressRecorder()
        let system = SFTPFileSystem(transportFactory: {
            if attempts.next() > 1 {
                guard stalled else { throw SFTPFailure.message("fixture cleanup unavailable") }
                return SFTPSSHTransport(executable: URL(fileURLWithPath: "/bin/sleep"), arguments: ["30"], environment: [:],
                    authenticationDirectory: cleanupAuth)
            }
            let transport = SFTPSSHTransport(executable: URL(fileURLWithPath: "/usr/libexec/sftp-server"),
                arguments: ["-d", root.path], environment: [:], authenticationDirectory: try self.temporaryDirectory())
            holder.set(transport); return transport
        })
        let began = ProcessInfo.processInfo.systemUptime
        var recoveryPath: String?
        do {
            try await system.transfer(direction: .upload, local: source, remote: "target", overwrite: true, progress: { sample in
                recorder.append(sample)
                if sample.bytesTransferred >= 32_768 { holder.cancel() }
            })
            XCTFail("Failed cleanup must remain an actionable failure")
        } catch SFTPFileSystemError.transferCleanupRequired(let staging, let reason) {
            recoveryPath = staging
            XCTAssertFalse(reason.isEmpty)
            if !stalled { XCTAssertEqual(reason, "fixture cleanup unavailable") }
        }
        let retained = try FileManager.default.contentsOfDirectory(atPath: root.path).filter { $0.hasPrefix(".nativepipe-transfer-") }
        XCTAssertEqual(retained.count, 1)
        XCTAssertEqual(recoveryPath, try canonicalPath(root) + "/" + (try XCTUnwrap(retained.first)))
        XCTAssertEqual(try String(contentsOf: target, encoding: .utf8), "original")
        XCTAssertTrue(recorder.samples.allSatisfy { !$0.isComplete })
        if stalled {
            XCTAssertLessThan(ProcessInfo.processInfo.systemUptime - began, 5,
                "The full cleanup must be bounded even though the new transport's ordinary response timeout is 45 seconds")
            XCTAssertFalse(FileManager.default.fileExists(atPath: cleanupAuth.path))
        }
    }
    @MainActor func testCleanupSSHOptionsAreNoninteractiveBeforeUserOptionsAndKeepEndpoint() throws {
        let command = SSHCommand(destination: "fixture@host-alias", application: ["true"],
            sshArguments: ["-p", "2222", "-F", "/authorized/config", "-i", "/authorized/key",
                           "-o", "BatchMode=no", "-o", "StrictHostKeyChecking=ask"])
        let arguments = SFTPFileSystem.subsystemArguments(command: command, environment: [:], cleanup: true)
        for (forced, user) in [("BatchMode=yes", "BatchMode=no"), ("StrictHostKeyChecking=yes", "StrictHostKeyChecking=ask")] {
            XCTAssertLessThan(try XCTUnwrap(arguments.firstIndex(of: forced)), try XCTUnwrap(arguments.firstIndex(of: user)))
        }
        XCTAssertTrue(arguments.contains("ConnectTimeout=2"))
        XCTAssertTrue(arguments.contains("NumberOfPasswordPrompts=0"))
        XCTAssertEqual(Array(arguments.suffix(command.sshArguments.count + 4)), command.sshArguments + ["-s", "--", command.destination, "sftp"])
    }
    func testTruncatedAttributesUnknownFlagsAndUnsafeNamesAreRejected() throws {
        XCTAssertThrowsError(try SFTPWire.packet(Data()))
        XCTAssertThrowsError(try SFTPWire.packet(Data(repeating: 0, count: SFTPWire.maximumPacket + 1)))
        for bytes in [Data(), Data([0, 0, 0, 1]), Data([0, 0, 0, 16]), Data([128, 0, 0, 0, 255, 255, 255, 255])] {
            var reader = SFTPReader(bytes); XCTAssertThrowsError(try reader.attributes())
        }
        for child in ["", ".", "..", "../escape", "slash/name", "nul\0"] { XCTAssertThrowsError(try SFTPPath.validateChild(child)) }
        for child in ["space name", "quote\"", "back\\slash", "*?[1]", "line\nname"] { XCTAssertNoThrow(try SFTPPath.validateChild(child)) }
    }
    func testPipelinedRepliesMayArriveOutOfOrderAndUnknownIDsFail() throws {
        let transport = SFTPReorderingFixture()
        let connection = try SFTPConnection(transport: transport)
        let first = try connection.enqueueWrite(Data([1]), offset: 0, data: Data([2]))
        let second = try connection.enqueueWrite(Data([1]), offset: 1, data: Data([3]))
        XCTAssertEqual(transport.requestCount, 2)
        try connection.acknowledgeWrite(first); try connection.acknowledgeWrite(second)
        XCTAssertEqual(transport.readCount, 3)
        transport.wrongIdentifier = true
        XCTAssertThrowsError(try connection.writeFile(Data([1]), offset: 2, data: Data([4])))
    }
    @MainActor func testDirectoryMetadataAvoidsPerEntryRoundTripsAndFallsBackWhenTypeMissing() async throws {
        for missingType in [false, true] {
            let transport = SFTPListingFixture(names: ["safe"], missingType: missingType)
            let system = SFTPFileSystem(transportFactory: { transport })
            let directory = try await system.list()
            XCTAssertEqual(directory.entries.first?.kind, .file)
            XCTAssertEqual(directory.entries.first?.size, 5)
            XCTAssertEqual(transport.lstatCount, missingType ? 1 : 0)
            XCTAssertTrue(transport.closed)
        }
    }
    @MainActor func testDirectoryListingRejectsTraversalAndDuplicateNamesAndClosesTransport() async throws {
        for names in [["../escape"], ["same", "same"], ["/absolute"], [""]] {
            let transport = SFTPListingFixture(names: names)
            let system = SFTPFileSystem(transportFactory: { transport })
            do { _ = try await system.list(); XCTFail("Unsafe directory entries must fail") }
            catch SFTPFailure.protocolError { }
            XCTAssertTrue(transport.closed)
        }
    }
    @MainActor func testDirectoryPipelineDrainsReorderedEOFAndNamesBeforeReuse() async throws {
        let transport = SFTPListingFixture(names: [], pages: [nil, ["first"], ["second"]], reverseResponses: true)
        let system = SFTPFileSystem(reuseConnection: true, transportFactory: { transport })
        let first = try await system.list()
        XCTAssertEqual(first.entries.map(\.name), ["first", "second"])
        XCTAssertEqual(transport.maximumPendingDirectoryReads, 8)
        XCTAssertEqual(transport.pendingReplies, 0, "EOF must not strand names or status replies on a reused stream")
        let second = try await system.list()
        XCTAssertEqual(second.entries.map(\.name), ["first", "second"])
        XCTAssertEqual(transport.pendingReplies, 0)
        system.close()
    }
    @MainActor func testDirectoryPipelineValidatesNamesAlreadySentWhenEOFIsObserved() async throws {
        let cases: [[[String]?]] = [[nil, ["../escape"]], [nil, ["same"], ["same"]]]
        for pages in cases {
            let transport = SFTPListingFixture(names: [], pages: pages)
            let system = SFTPFileSystem(reuseConnection: true, transportFactory: { transport })
            do { _ = try await system.list(); XCTFail("Pending replies after EOF must still be validated") }
            catch SFTPFailure.protocolError { }
            XCTAssertTrue(transport.closed)
        }
    }
    @MainActor func testDirectoryListingKeepsLinuxUnicodeAndCaseVariantsDistinctByUTF8() async throws {
        let names = ["Résumé.txt", "Re\u{301}sume\u{301}.txt", "Case.txt", "case.txt"]
        XCTAssertEqual(names[0], names[1], "Swift canonical equality is the regression trigger")
        let transport = SFTPListingFixture(names: names)
        let system = SFTPFileSystem(transportFactory: { transport })
        let directory = try await system.list()
        XCTAssertEqual(directory.entries.count, names.count)
        XCTAssertEqual(Set(directory.entries.map { Data($0.name.utf8) }), Set(names.map { Data($0.utf8) }))
        XCTAssertTrue(transport.closed)
    }
    @MainActor func testLocalUnicodeAndCaseCollisionsFailWithoutReplacingExistingTree() async throws {
        let local = try temporaryDirectory(); defer { try? FileManager.default.removeItem(at: local) }
        for names in [["Résumé.txt", "Re\u{301}sume\u{301}.txt"], ["Case.txt", "case.txt"]] {
            let probe = local.appendingPathComponent("probe-" + UUID().uuidString)
            try FileManager.default.createDirectory(at: probe, withIntermediateDirectories: false)
            let first = Darwin.open(probe.path + "/" + names[0], O_WRONLY | O_CREAT | O_EXCL, 0o600)
            XCTAssertGreaterThanOrEqual(first, 0); if first >= 0 { Darwin.close(first) }
            let second = Darwin.open(probe.path + "/" + names[1], O_WRONLY | O_CREAT | O_EXCL, 0o600)
            let collides = second < 0 && errno == EEXIST
            if second >= 0 { Darwin.close(second) }
            try FileManager.default.removeItem(at: probe)
            let destination = local.appendingPathComponent("target-" + UUID().uuidString)
            try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: false)
            let original = destination.appendingPathComponent("original")
            try Data("preserved".utf8).write(to: original)
            let before = try FileManager.default.attributesOfItem(atPath: destination.path)
            let transport = SFTPListingFixture(names: names, directoryPath: "/fixture/source", emptyFiles: true)
            let system = SFTPFileSystem(transportFactory: { transport }), recorder = SFTPProgressRecorder()
            do {
                try await system.transfer(direction: .download, local: destination, remote: "source", overwrite: true, progress: { [recorder] sample in recorder.append(sample) })
                XCTAssertFalse(collides, "A filesystem alias must never silently overwrite an earlier staged file")
                XCTAssertEqual(Set(try rawNames(destination).map { Data($0) }), Set(names.map { Data($0.utf8) }))
            } catch SFTPFileSystemError.localNameCollision {
                XCTAssertTrue(collides)
                XCTAssertEqual(try String(contentsOf: original, encoding: .utf8), "preserved")
                let after = try FileManager.default.attributesOfItem(atPath: destination.path)
                XCTAssertEqual(after[.modificationDate] as? Date, before[.modificationDate] as? Date)
                XCTAssertEqual(after[.posixPermissions] as? NSNumber, before[.posixPermissions] as? NSNumber)
                XCTAssertTrue(recorder.samples.allSatisfy { !$0.isComplete })
            }
            XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: local.path).contains { $0.hasPrefix(".nativepipe-transfer-") })
            XCTAssertTrue(transport.closed)
        }
    }
    @MainActor func testCancellationAndClientCloseDuringPublicationCommitCompleteDestination() async throws {
        try await exercisePublicationCancellation(.commit)
    }
    @MainActor func testCancellationAndClientCloseDuringFailedPublicationRestoreOriginal() async throws {
        try await exercisePublicationCancellation(.restore)
    }
    @MainActor func testFailedRollbackRetainsTypedRecoveryPathsDespiteTaskCancellation() async throws {
        try await exercisePublicationCancellation(.recoveryRequired)
    }
    @MainActor private func exercisePublicationCancellation(_ mode: SFTPReplacementControl.Mode) async throws {
        guard FileManager.default.isExecutableFile(atPath: "/usr/libexec/sftp-server") else { throw XCTSkip("The system SFTP server is unavailable.") }
        let root = try temporaryDirectory(), local = try temporaryDirectory(), auth = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: local); try? FileManager.default.removeItem(at: auth) }
        let source = local.appendingPathComponent("source"), target = root.appendingPathComponent("target")
        try FileManager.default.createDirectory(at: source, withIntermediateDirectories: false)
        try FileManager.default.createDirectory(at: target, withIntermediateDirectories: false)
        try Data("new".utf8).write(to: source.appendingPathComponent("new"))
        try Data("original".utf8).write(to: target.appendingPathComponent("old"))
        let control = SFTPReplacementControl(mode: mode), taskHolder = SFTPTaskHolder(), recorder = SFTPProgressRecorder()
        let attempts = SFTPAttemptCounter()
        let system = SFTPFileSystem(transportFactory: {
            // Closing the first transport removes its authentication attempt.
            // A cleanup connection needs a fresh attempt, as production does.
            let authentication = try (attempts.next() == 1 ? auth : self.temporaryDirectory())
            return SFTPReplacementFixture(base: SFTPSSHTransport(executable: URL(fileURLWithPath: "/usr/libexec/sftp-server"),
                arguments: ["-d", root.path], environment: [:], authenticationDirectory: authentication), control: control)
        })
        control.onOriginalMoved = {
            taskHolder.cancel()
            let completedClose = DispatchSemaphore(value: 0)
            Task { @MainActor in system.close(); completedClose.signal() }
            control.clientCloseCompleted = completedClose.wait(timeout: .now() + 2) == .success
            control.authenticationKeptDuringPublication = FileManager.default.fileExists(atPath: auth.path)
        }
        let operation = Task { try await system.transfer(direction: .upload, local: source, remote: "target", overwrite: true, progress: { [recorder] sample in recorder.append(sample) }) }
        taskHolder.set(operation)
        var failure: Error?
        do { try await operation.value } catch { failure = error }
        control.onOriginalMoved = nil
        XCTAssertTrue(operation.isCancelled)
        XCTAssertTrue(control.clientCloseCompleted)
        XCTAssertTrue(control.authenticationKeptDuringPublication, "Cancel/close must not tear down a publication transaction")
        XCTAssertFalse(FileManager.default.fileExists(atPath: auth.path))
        let backup = try XCTUnwrap(control.backupPath)
        switch mode {
        case .commit:
            XCTAssertNil(failure)
            XCTAssertEqual(try String(contentsOf: target.appendingPathComponent("new"), encoding: .utf8), "new")
            XCTAssertEqual(try String(contentsOfFile: backup + "/old", encoding: .utf8), "original")
            XCTAssertEqual(recorder.samples.last?.isComplete, true)
            XCTAssertEqual(recorder.samples.last?.bytesTransferred, 3)
        case .restore:
            guard let serverFailure = failure as? SFTPFailure, case .status(let code, _) = serverFailure else {
                XCTFail("The rejected publication must remain a server failure"); return
            }
            XCTAssertEqual(code, 3)
            XCTAssertEqual(try String(contentsOf: target.appendingPathComponent("old"), encoding: .utf8), "original")
            XCTAssertFalse(FileManager.default.fileExists(atPath: backup))
            XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: root.path).contains { $0.hasPrefix(".nativepipe-transfer-") })
            XCTAssertTrue(recorder.samples.allSatisfy { !$0.isComplete })
        case .recoveryRequired:
            guard let fileFailure = failure as? SFTPFileSystemError,
                  case .replacementRecoveryRequired(let destination, let originalBackup) = fileFailure else {
                XCTFail("Task cancellation must not erase recovery diagnostics"); return
            }
            XCTAssertEqual(destination, try canonicalPath(root) + "/target")
            XCTAssertEqual(originalBackup, backup)
            XCTAssertEqual(try String(contentsOfFile: originalBackup + "/old", encoding: .utf8), "original")
            XCTAssertTrue(failure?.localizedDescription.contains(originalBackup) == true)
            XCTAssertTrue(recorder.samples.allSatisfy { !$0.isComplete })
        }
    }
    func testFailedWriteIsNeverAnAcknowledgment() throws {
        let transport = SFTPReorderingFixture(); transport.statusCode = 3
        let connection = try SFTPConnection(transport: transport)
        XCTAssertThrowsError(try connection.writeFile(Data([1]), offset: 0, data: Data([2])))
    }
    func testRealProcessTimeoutCancelsAndClosesAuthenticationAttempt() throws {
        let auth = try temporaryDirectory()
        let transport = SFTPSSHTransport(executable: URL(fileURLWithPath: "/bin/sleep"), arguments: ["30"], environment: [:],
            authenticationDirectory: auth, timeout: 0.15)
        defer { transport.close() }
        let began = ProcessInfo.processInfo.systemUptime
        XCTAssertThrowsError(try SFTPConnection(transport: transport))
        transport.cancel(); transport.close()
        XCTAssertLessThan(ProcessInfo.processInfo.systemUptime - began, 2)
        XCTAssertFalse(FileManager.default.fileExists(atPath: auth.path))
    }
    func testAuthenticationCancellationStopsWithoutWaitingForSSHExit() throws {
        for diagnosticCancellation in [true, false] {
            let auth = try temporaryDirectory()
            let script = diagnosticCancellation
                ? "printf 'NATIVEPIPE AUTH CANCELLED\\n' >&2; exec /bin/sleep 30"
                : "/bin/rmdir " + SSHCommand.quote(auth.path) + "; exec /bin/sleep 30"
            let transport = SFTPSSHTransport(executable: URL(fileURLWithPath: "/bin/sh"), arguments: ["-c", script],
                environment: [:], authenticationDirectory: auth)
            defer { transport.close() }
            let began = ProcessInfo.processInfo.systemUptime
            do { _ = try SFTPConnection(transport: transport); XCTFail("Authentication cancellation must throw") }
            catch is CancellationError { }
            XCTAssertLessThan(ProcessInfo.processInfo.systemUptime - began, 2)
            XCTAssertFalse(FileManager.default.fileExists(atPath: auth.path))
        }
    }
    @MainActor func testTaskCancellationTerminatesStalledSSHAndEscalatesPastIgnoredTERM() async throws {
        let directory = try temporaryDirectory(); defer { try? FileManager.default.removeItem(at: directory) }
        let auth = directory.appendingPathComponent("authentication"), marker = directory.appendingPathComponent("pid")
        try FileManager.default.createDirectory(at: auth, withIntermediateDirectories: false)
        let system = SFTPFileSystem(transportFactory: {
            SFTPSSHTransport(executable: URL(fileURLWithPath: "/bin/sh"),
                arguments: ["-c", "trap '' TERM; echo $$ > " + SSHCommand.quote(marker.path) + "; exec /bin/sleep 30"],
                environment: [:], authenticationDirectory: auth)
        })
        let operation = Task { try await system.list() }
        let deadline = ProcessInfo.processInfo.systemUptime + 3
        while !FileManager.default.fileExists(atPath: marker.path), ProcessInfo.processInfo.systemUptime < deadline {
            try await Task.sleep(for: .milliseconds(10))
        }
        let pid = try XCTUnwrap(Int32(try String(contentsOf: marker, encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines)))
        let began = ProcessInfo.processInfo.systemUptime
        operation.cancel()
        do { _ = try await operation.value; XCTFail("Task cancellation must throw") } catch is CancellationError { }
        XCTAssertFalse(FileManager.default.fileExists(atPath: auth.path))
        while kill(pid, 0) == 0, ProcessInfo.processInfo.systemUptime - began < 3 { try await Task.sleep(for: .milliseconds(20)) }
        XCTAssertEqual(kill(pid, 0), -1, "A child ignoring TERM must still exit")
        XCTAssertLessThan(ProcessInfo.processInfo.systemUptime - began, 3)
    }
    @MainActor func testRemoteDesktopUserFilesUseTheBrowserStreamingEngine() async throws {
        let root = try temporaryDirectory(), local = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: local) }
        let system = try realServer(root, reuseConnection: true)
        let files = RemoteUserFileAccess(makeSession: { system })
        let source = local.appendingPathComponent("selected folder")
        try FileManager.default.createDirectory(at: source, withIntermediateDirectories: false)
        let bytes = Data(repeating: 0x51, count: 32 * 1024 * 1024 + 13)
        try bytes.write(to: source.appendingPathComponent("payload"))
        let imported = try await files.importFiles([source], shareDirectories: false)
        defer { try? FileManager.default.removeItem(at: imported[0].deletingLastPathComponent()) }
        let destination = local.appendingPathComponent("received")
        let recorder = SFTPProgressRecorder()
        try await files.exportFile(imported[0], to: destination, progress: { recorder.append($0) })
        XCTAssertEqual(try Data(contentsOf: destination.appendingPathComponent("payload")), bytes)
        XCTAssertEqual(recorder.samples.last?.bytesTransferred, UInt64(bytes.count))
        XCTAssertEqual(recorder.samples.last?.totalBytes, UInt64(bytes.count))
        XCTAssertEqual(recorder.samples.last?.isComplete, true)
        XCTAssertTrue(recorder.samples.contains { $0.relativePath == "payload" })
        do { try await files.exportFile(imported[0], to: destination); XCTFail("An existing destination must not be replaced") }
        catch SFTPFileSystemError.destinationExists { }
    }

    @MainActor func testFailedWritePreservesOriginalAndReportsOnlySuccessfulAcknowledgments() async throws {
        let root = try temporaryDirectory(), local = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: local) }
        guard FileManager.default.isExecutableFile(atPath: "/usr/libexec/sftp-server") else { throw XCTSkip("The system SFTP server is unavailable.") }
        let source = local.appendingPathComponent("source"), target = root.appendingPathComponent("target")
        try Data(repeating: 1, count: 1_048_576).write(to: source); try Data("old".utf8).write(to: target)
        let system = SFTPFileSystem(transportFactory: {
            let auth = try self.temporaryDirectory()
            return SFTPFailedWriteFixture(base: SFTPSSHTransport(executable: URL(fileURLWithPath: "/usr/libexec/sftp-server"),
                arguments: ["-d", root.path], environment: [:], authenticationDirectory: auth), failureAt: 4)
        })
        let recorder = SFTPProgressRecorder()
        do { try await system.transfer(direction: .upload, local: source, remote: "target", overwrite: true, progress: { [recorder] sample in recorder.append(sample) }); XCTFail("Server failure must throw") }
        catch SFTPFailure.status(let code, _) { XCTAssertEqual(code, 3) }
        XCTAssertEqual(recorder.samples.first { $0.bytesTransferred > 0 }?.bytesTransferred, 32_768)
        XCTAssertEqual(recorder.samples.last?.bytesTransferred, 3 * 32_768, "Failure must flush acknowledged bytes suppressed by throttling")
        XCTAssertTrue(recorder.samples.allSatisfy { !$0.isComplete })
        XCTAssertEqual(try String(contentsOf: target, encoding: .utf8), "old")
        XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: root.path).contains { $0.hasPrefix(".nativepipe-transfer-") })
    }
}

private final class SFTPProgressRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var values: [FileTransferProgress] = []
    var samples: [FileTransferProgress] { lock.lock(); defer { lock.unlock() }; return values }
    func append(_ value: FileTransferProgress) { lock.lock(); values.append(value); lock.unlock() }
}

private final class SFTPTransportHolder: @unchecked Sendable {
    private let lock = NSLock()
    private var value: SFTPTransport?
    func set(_ value: SFTPTransport) { lock.lock(); self.value = value; lock.unlock() }
    func cancel() { lock.lock(); let current = value; lock.unlock(); current?.cancel() }
}

private final class SFTPAttemptCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0
    func next() -> Int { lock.lock(); defer { lock.unlock() }; count += 1; return count }
}

private final class SFTPReorderingFixture: SFTPTransport, @unchecked Sendable {
    var requests: [UInt32] = []
    var requestCount = 0, readCount = 0
    var wrongIdentifier = false
    var statusCode: UInt32 = 0
    private var version = true
    func start() throws { }
    func checkCancellation() throws { }
    func cancel() { }
    func close() { }
    func write(_ data: Data) throws {
        var packet = SFTPReader(data); _ = try packet.uint32()
        guard try packet.byte() != 1 else { return }
        requests.append(try packet.uint32()); requestCount += 1
    }
    func readPacket() throws -> Data {
        readCount += 1
        if version { version = false; var result = Data([2]); result.sftpUInt32(3); return result }
        let id = requests.removeLast()
        var result = Data([101]); result.sftpUInt32(wrongIdentifier ? id + 100 : id)
        result.sftpUInt32(statusCode); result.sftpString(statusCode == 0 ? "" : "denied"); result.sftpString("")
        return result
    }
}

private final class SFTPFailedWriteFixture: SFTPTransport, @unchecked Sendable {
    let base: SFTPTransport
    let failureAt: Int
    private var writes = 0
    private var failedID: UInt32?
    init(base: SFTPTransport, failureAt: Int = 2) { self.base = base; self.failureAt = failureAt }
    func start() throws { try base.start() }
    func checkCancellation() throws { try base.checkCancellation() }
    func cancel() { base.cancel() }
    func close() { base.close() }
    func write(_ data: Data) throws {
        var reader = SFTPReader(data); _ = try reader.uint32()
        if try reader.byte() == 6 {
            writes += 1
            if writes == failureAt { failedID = try reader.uint32() }
        }
        try base.write(data)
    }
    func readPacket() throws -> Data {
        let result = try base.readPacket()
        var reader = SFTPReader(result)
        if try reader.byte() == 101, try reader.uint32() == failedID {
            var failure = Data([101]); failure.sftpUInt32(failedID!); failure.sftpUInt32(3)
            failure.sftpString("fixture write rejection"); failure.sftpString(""); return failure
        }
        return result
    }
}

private final class SFTPListingFixture: SFTPTransport, @unchecked Sendable {
    enum CloseFailure { case refused, timeout }
    let names: [String]
    let missingType: Bool
    let directoryPath: String
    let emptyFiles: Bool
    let includeTimes: Bool
    let preserveCanonicalPaths: Bool
    private var response = Data()
    private var responses: [Data] = []
    private var sentEntries = false
    private let pages: [[String]?]?
    private let reverseResponses: Bool
    private var page = 0
    private var pendingDirectoryReads = Set<UInt32>()
    private(set) var maximumPendingDirectoryReads = 0
    private(set) var lstatCount = 0
    private(set) var realpathCount = 0
    private(set) var closed = false
    private var failNext = false
    private let closeFailure: CloseFailure?
    private let failStat: Bool
    private var failedCloseID: UInt32?
    private(set) var closeRequests = 0
    var pendingReplies: Int { responses.count }
    init(names: [String], missingType: Bool = false, directoryPath: String = "/fixture", emptyFiles: Bool = false,
         pages: [[String]?]? = nil, reverseResponses: Bool = false, closeFailure: CloseFailure? = nil,
         failStat: Bool = false, includeTimes: Bool = false, preserveCanonicalPaths: Bool = false) {
        self.names = names; self.missingType = missingType; self.directoryPath = directoryPath; self.emptyFiles = emptyFiles
        self.pages = pages; self.reverseResponses = reverseResponses
        self.closeFailure = closeFailure
        self.failStat = failStat
        self.includeTimes = includeTimes; self.preserveCanonicalPaths = preserveCanonicalPaths
    }
    func start() throws { }
    func checkCancellation() throws { }
    func cancel() { }
    func close() { closed = true }
    func failNextRequest() { failNext = true }
    private func attributes(directory: Bool, missingType: Bool = false) -> Data {
        var value = Data(); value.sftpUInt32((missingType ? 1 : 5) | (includeTimes ? 8 : 0)); value.sftpUInt64(directory || emptyFiles ? 0 : 5)
        if !missingType { value.sftpUInt32(directory ? 0o040700 : 0o100600) }
        if includeTimes { value.sftpUInt32(1_700_000_000); value.sftpUInt32(1_700_000_000) }
        return value
    }
    private func status(_ id: UInt32, _ code: UInt32) -> Data {
        var result = Data([101]); result.sftpUInt32(id); result.sftpUInt32(code); result.sftpString(""); result.sftpString(""); return result
    }
    func write(_ data: Data) throws {
        if failNext { failNext = false; throw SFTPFailure.protocolError("Injected failed stream.") }
        defer { responses.append(response) }
        var reader = SFTPReader(data); _ = try reader.uint32(); let type = try reader.byte()
        if type == 1 { response = Data([2]); response.sftpUInt32(3); return }
        let id = try reader.uint32()
        switch type {
        case 16:
            realpathCount += 1
            let requested = try reader.string()
            response = Data([104]); response.sftpUInt32(id); response.sftpUInt32(1)
            response.sftpString(preserveCanonicalPaths && requested != "." ? requested : "/fixture")
            response.sftpString(""); response.append(attributes(directory: true))
        case 7:
            lstatCount += 1; let path = try reader.string()
            if failStat { response = status(id, 2); return }
            response = Data([105]); response.sftpUInt32(id); response.append(attributes(directory: path == "/fixture" || path == directoryPath))
        case 11:
            sentEntries = false; page = 0
            response = Data([102]); response.sftpUInt32(id); response.sftpBytes(Data([0, 255, 1]))
        case 12:
            pendingDirectoryReads.insert(id)
            maximumPendingDirectoryReads = max(maximumPendingDirectoryReads, pendingDirectoryReads.count)
            let batch: [String]?
            if let pages { batch = page < pages.count ? pages[page] : nil; page += 1 }
            else { batch = sentEntries ? nil : names; sentEntries = true }
            guard let batch else { response = status(id, 1); return }
            response = Data([104]); response.sftpUInt32(id); response.sftpUInt32(UInt32(batch.count))
            for name in batch { response.sftpString(name); response.sftpString(""); response.append(attributes(directory: false, missingType: missingType)) }
        case 4:
            closeRequests += 1
            if closeFailure == .timeout { failedCloseID = id }
            response = status(id, closeFailure == .refused ? 3 : 0)
        case 3:
            response = Data([102]); response.sftpUInt32(id); response.sftpBytes(Data([2]))
        case 8:
            response = Data([105]); response.sftpUInt32(id); response.append(attributes(directory: false))
        case 5: response = status(id, 1)
        default: throw SFTPFailure.protocolError("Unexpected fixture request.")
        }
    }
    func readPacket() throws -> Data {
        guard !responses.isEmpty else { throw SFTPFailure.protocolError("Missing fixture response.") }
        if let failedCloseID {
            var reader = SFTPReader(reverseResponses ? responses.last! : responses.first!)
            if try reader.byte() != 2, try reader.uint32() == failedCloseID {
                throw SFTPFailure.message("Injected CLOSE timeout.")
            }
        }
        let response = reverseResponses ? responses.removeLast() : responses.removeFirst()
        var reader = SFTPReader(response)
        if try reader.byte() != 2 { pendingDirectoryReads.remove(try reader.uint32()) }
        return response
    }
}

private final class SFTPTransportRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var transports: [SFTPTransport] = []
    func append(_ transport: SFTPTransport) { lock.lock(); defer { lock.unlock() }; transports.append(transport) }
    var count: Int { lock.lock(); defer { lock.unlock() }; return transports.count }
}

private final class SFTPListingSessionRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var transports: [SFTPListingFixture] = []
    private let firstCloseFailure: SFTPListingFixture.CloseFailure?
    init(firstCloseFailure: SFTPListingFixture.CloseFailure? = nil) { self.firstCloseFailure = firstCloseFailure }
    func makeTransport() -> SFTPTransport {
        lock.lock(); defer { lock.unlock() }
        let transport = SFTPListingFixture(names: ["entry"], closeFailure: transports.isEmpty ? firstCloseFailure : nil)
        transports.append(transport); return transport
    }
    var count: Int { lock.lock(); defer { lock.unlock() }; return transports.count }
    var first: SFTPListingFixture { lock.lock(); defer { lock.unlock() }; return transports.first! }
    var last: SFTPListingFixture { lock.lock(); defer { lock.unlock() }; return transports.last! }
}

private final class SFTPTaskHolder: @unchecked Sendable {
    private let lock = NSLock()
    private var value: Task<Void, Error>?
    func set(_ value: Task<Void, Error>) { lock.lock(); self.value = value; lock.unlock() }
    func cancel() { lock.lock(); let task = value; lock.unlock(); task?.cancel() }
}

private final class SFTPReplacementControl: @unchecked Sendable {
    enum Mode { case commit, restore, recoveryRequired }
    let mode: Mode
    var onOriginalMoved: (@Sendable () -> Void)?
    var backupPath: String?
    var clientCloseCompleted = false
    var authenticationKeptDuringPublication = false
    init(mode: Mode) { self.mode = mode }
}

private final class SFTPReplacementFixture: SFTPTransport, @unchecked Sendable {
    let base: SFTPTransport
    let control: SFTPReplacementControl
    private var renameCount = 0
    private var firstRenameID: UInt32?
    private var syntheticResponse: Data?
    init(base: SFTPTransport, control: SFTPReplacementControl) { self.base = base; self.control = control }
    func start() throws { try base.start() }
    func checkCancellation() throws { try base.checkCancellation() }
    func beginPublication() throws { try base.beginPublication() }
    func endPublication() { base.endPublication() }
    func cancel() { base.cancel() }
    func close() { base.close() }
    func write(_ data: Data) throws {
        var reader = SFTPReader(data); _ = try reader.uint32()
        if try reader.byte() == 18 {
            renameCount += 1; let id = try reader.uint32()
            _ = try reader.string(); let destination = try reader.string()
            if renameCount == 1 { firstRenameID = id; control.backupPath = destination }
            if renameCount == 2 && control.mode != .commit || renameCount == 3 && control.mode == .recoveryRequired {
                var failure = Data([101]); failure.sftpUInt32(id); failure.sftpUInt32(3)
                failure.sftpString("fixture publication rejection"); failure.sftpString("")
                syntheticResponse = failure; return
            }
        }
        try base.write(data)
    }
    func readPacket() throws -> Data {
        if let response = syntheticResponse { syntheticResponse = nil; return response }
        let response = try base.readPacket()
        var reader = SFTPReader(response)
        if try reader.byte() == 101, try reader.uint32() == firstRenameID, try reader.uint32() == 0 {
            firstRenameID = nil; control.onOriginalMoved?()
        }
        return response
    }
}
