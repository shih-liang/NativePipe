import Foundation
import Darwin
import FSKit
import NativePipeFileSharing
import XCTest
@testable import NativePipeFileSystem

@available(macOS 27.0, *)
final class SharedFilesVolumeTests: XCTestCase {
    override func setUpWithError() throws {
        try super.setUpWithError()
        guard ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27 else {
            throw XCTSkip("Native FSKit sharing requires macOS 27 or later")
        }
    }

    private func item(id: UInt64 = 3, kind: String = "file", permissions: UInt32 = 0o755,
                      size: UInt64 = 5 * 1024 * 1024 * 1024, version: String = "AQ==") throws -> SharedFileItem {
        try JSONDecoder().decode(SharedFileItem.self, from: Data("""
        {"id":\(id),"parentID":2,"name":"中文.txt","kind":"\(kind)","size":\(size),"permissions":\(permissions),"version":"\(version)"}
        """.utf8))
    }

    func testLargeFileMetadataPreservesSizeAndApprovedExecutableBits() throws {
        let metadata = try item()
        let attributes = try SharedFilesVolume.attributes(for: metadata)
        XCTAssertEqual(attributes.size, 5 * 1024 * 1024 * 1024)
        XCTAssertEqual(attributes.mode, 0o500)
        XCTAssertEqual(attributes.fileID.rawValue, 3)
        XCTAssertEqual(attributes.parentID.rawValue, 2)
        XCTAssertEqual(attributes.allocSize, 0)
        XCTAssertTrue(attributes.supportsLimitedXAttrs)
        XCTAssertTrue(attributes.inhibitKernelOffloadedIO)
    }

    func testReadOnlyPermissionsDoNotInventExecuteAccess() throws {
        let attributes = try SharedFilesVolume.attributes(for: item(permissions: 0o600))
        XCTAssertEqual(attributes.mode, 0o400)
        XCTAssertEqual(attributes.type, .file)
        let directory = try SharedFilesVolume.attributes(for: item(kind: "directory", permissions: 0o700, size: 0))
        XCTAssertEqual(directory.mode, 0o500)
        XCTAssertEqual(directory.type, .directory)
    }

    func testEnumerationVerifierChangesWhenSelectionOrSourceVersionChanges() throws {
        let first = try item()
        let changed = try item(version: "Ag==")
        let other = try item(id: 4)
        XCTAssertNotEqual(SharedFilesVolume.directoryVerifier([first]), SharedFilesVolume.directoryVerifier([changed]))
        XCTAssertNotEqual(SharedFilesVolume.directoryVerifier([first]), SharedFilesVolume.directoryVerifier([first, other]))
        XCTAssertNotEqual(SharedFilesVolume.directoryVerifier([]), 0)
    }

    func testMalformedRemoteMetadataCannotTrapTheExtension() throws {
        for seconds in [Double.nan, Double.infinity, Double(Int.max)] {
            let metadata = SharedFileItem(id: 3, parentID: 2, name: "invalid", kind: .file,
                size: 0, modified: Date(timeIntervalSince1970: seconds), permissions: 0o644, version: Data([1]))
            XCTAssertThrowsError(try SharedFilesVolume.attributes(for: metadata))
        }
        XCTAssertThrowsError(try SharedFilesVolume.attributes(for: item(size: UInt64.max)))
    }

    func testNativeAccessCheckRestrictsSharedDataToItsMacOwner() throws {
        let file = try item()
        XCTAssertTrue(SharedFilesVolume.allowsAccess(file, requested: [.readData, .execute], callerUID: Int(getuid())))
        XCTAssertFalse(SharedFilesVolume.allowsAccess(file, requested: .readData, callerUID: Int(getuid()) + 1))
        XCTAssertFalse(SharedFilesVolume.allowsAccess(file, requested: .writeData, callerUID: Int(getuid())))
        XCTAssertFalse(SharedFilesVolume.allowsAccess(try item(permissions: 0o644), requested: .execute, callerUID: Int(getuid())))
    }
}
