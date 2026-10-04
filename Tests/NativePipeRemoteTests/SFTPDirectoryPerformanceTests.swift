import Foundation
import XCTest
import Darwin
@testable import NativePipeRemote

final class SFTPDirectoryPerformanceTests: XCTestCase {
    func testLargeDirectoryReportsProtocolAndLocalCosts() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("nativepipe-directory-cost-" + UUID().uuidString)
        let auth = FileManager.default.temporaryDirectory.appendingPathComponent("nativepipe-directory-auth-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
        try FileManager.default.createDirectory(at: auth, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: root); try? FileManager.default.removeItem(at: auth) }
        for index in 0..<6_222 {
            let path = root.path + "/command-" + String(index)
            let fd = open(path, O_CREAT | O_WRONLY | O_EXCL, 0o600)
            guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
            close(fd)
        }
        let base = SFTPSSHTransport(executable: URL(fileURLWithPath: "/usr/libexec/sftp-server"), arguments: ["-d", root.path],
            environment: ProcessInfo.processInfo.environment, authenticationDirectory: auth)
        let transport = SFTPDirectoryCostTransport(base: base)
        defer { transport.close() }
        let connection = try SFTPConnection(transport: transport)
        let path = try connection.realpath(".")
        let initialIO = transport.ioSeconds, initialPackets = transport.readCount
        let started = ProcessInfo.processInfo.systemUptime
        let entries = try connection.entries(path)
        let elapsed = ProcessInfo.processInfo.systemUptime - started
        XCTAssertEqual(entries.count, 6_222)
        XCTAssertEqual(Set(entries.map { Data($0.0.utf8) }).count, 6_222)
        XCTAssertTrue(entries.allSatisfy { $0.1.hasKind && $0.1.kind == .file && $0.1.size == 0 })
        print("SFTP_DIRECTORY_COST entries=\(entries.count) elapsed=\(elapsed) wire=\(transport.ioSeconds - initialIO) readdir=\(transport.readDirectoryCount) packets=\(transport.readCount - initialPackets)")
    }
}

private final class SFTPDirectoryCostTransport: SFTPTransport, @unchecked Sendable {
    let base: SFTPTransport
    private(set) var ioSeconds: TimeInterval = 0
    private(set) var readDirectoryCount = 0
    private(set) var readCount = 0
    init(base: SFTPTransport) { self.base = base }
    func start() throws { try base.start() }
    func write(_ data: Data) throws {
        var reader = SFTPReader(data); _ = try reader.uint32()
        if try reader.byte() == 12 { readDirectoryCount += 1 }
        let started = ProcessInfo.processInfo.systemUptime
        defer { ioSeconds += ProcessInfo.processInfo.systemUptime - started }
        try base.write(data)
    }
    func readPacket() throws -> Data {
        let started = ProcessInfo.processInfo.systemUptime
        defer { ioSeconds += ProcessInfo.processInfo.systemUptime - started; readCount += 1 }
        return try base.readPacket()
    }
    func checkCancellation() throws { try base.checkCancellation() }
    func cancel() { base.cancel() }
    func close() { base.close() }
}
