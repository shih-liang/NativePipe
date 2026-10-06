import Foundation
import NativePipeStrings
import XCTest
@testable import NativePipeRemote

/// Linux-side messages exist twice: translated in RemoteText, and as English
/// fallbacks inside each reader (the compositor's C, the installer, the SSH
/// bootstraps). Nothing ties the two together at build time, so these tests do.
final class RemoteTextTests: XCTestCase {
    private static let root = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()

    private static func read(_ path: String) throws -> String {
        try String(contentsOf: root.appendingPathComponent(path), encoding: .utf8)
    }

    private static func matches(_ pattern: String, in text: String) throws -> [(String, String)] {
        let expression = try NSRegularExpression(pattern: pattern)
        return expression.matches(in: text, range: NSRange(text.startIndex..., in: text)).map {
            (String(text[Range($0.range(at: 1), in: text)!]), String(text[Range($0.range(at: 2), in: text)!]))
        }
    }

    /// Name and English key of every exported message, read from the source so
    /// the check does not depend on the language the tests happen to run in.
    private static func exported() throws -> [String: String] {
        let source = try read("Sources/NativePipeRemote/RemoteText.swift")
        let pairs = try matches(#"\("([A-Z0-9_]+)", NPText\("((?:[^"\\]|\\.)*)"\)\)"#, in: source)
        return Dictionary(pairs, uniquingKeysWith: { first, _ in first })
    }

    /// Every fallback a reader prints when the Mac exported nothing.
    private static func readers() throws -> [(where: String, name: String, english: String)] {
        var result: [(String, String, String)] = []
        let compositor = root.appendingPathComponent("guest/compositor")
        let files = try XCTUnwrap(FileManager.default.enumerator(at: compositor, includingPropertiesForKeys: nil))
        for case let file as URL in files where file.pathExtension == "c" && !file.path.contains("/tests/") {
            let text = try String(contentsOf: file, encoding: .utf8)
            for (name, english) in try matches(#"np_user_text\("([A-Z0-9_]+)", "((?:[^"\\]|\\.)*)""#, in: text) {
                result.append((file.lastPathComponent, name, english))
            }
        }
        let installer = try read("scripts/install-compositor.sh")
        for (name, english) in try matches(#"say ([A-Z0-9_]+) "([^"]*)""#, in: installer) {
            result.append(("install-compositor.sh", name, english))
        }
        let bootstraps = try read("Sources/NativePipeRemote/RemoteCompositorInstaller.swift")
        for (name, english) in try matches(#"\$\{NATIVEPIPE_TEXT_([A-Z0-9_]+):-([^}]*)\}"#, in: bootstraps) {
            result.append(("RemoteCompositorInstaller.swift", name, english))
        }
        return result
    }

    func testEveryReaderFallbackMatchesItsExportedMessage() throws {
        let exported = try Self.exported()
        let readers = try Self.readers()
        XCTAssertGreaterThan(exported.count, 15, "scan of RemoteText.swift found too little")
        XCTAssertGreaterThan(readers.count, 15, "scan of the readers found too little")
        for reader in readers {
            guard let key = exported[reader.name] else {
                XCTFail("\(reader.where): \(reader.name) is never exported, so it can never be translated")
                continue
            }
            XCTAssertEqual(key.replacingOccurrences(of: "%@", with: "%s"), reader.english,
                           "\(reader.where): \(reader.name) English differs from RemoteText")
        }
        let read = Set(readers.map(\.name))
        for name in exported.keys where !read.contains(name) {
            XCTFail("\(name) is exported but no reader prints it")
        }
    }

    func testPositionalArgumentsAreKeptForCButNotForTheShell() {
        XCTAssertEqual(RemoteText.remoteNotation(name: "CANNOT_LAUNCH", text: "%2$@ – %1$@"), "%2$s – %1$s")
        XCTAssertEqual(RemoteText.remoteNotation(name: "CANNOT_LAUNCH", text: "a %@ b %@"), "a %s b %s")
        // The installer's say() substitutes in order only: English beats a
        // translation with its arguments swapped.
        XCTAssertNil(RemoteText.remoteNotation(name: "INSTALL_INSTALLING", text: "%2$@ (%1$@)"))
    }

    /// The export lines go through real quoting into a real shell and come out
    /// unchanged, in scripts that use curly apostrophes and CJK, and the
    /// installer's own say() then substitutes into the translation.
    func testTranslationsSurviveExportAndTheInstallerSubstitutesThem() throws {
        let exported = try Self.exported()
        let installer = try Self.read("scripts/install-compositor.sh")
        let say = try XCTUnwrap(installer.range(of: #"(?s)    say\(\) \{.*?\n    \}"#, options: .regularExpression)
            .map { String(installer[$0]) })
        for language in ["en", "zh-Hans", "ja", "fr"] {
            var exports: [String] = [], expected: [String] = []
            for (name, key) in exported.sorted(by: { $0.key < $1.key }) {
                let text = NPStrings.text(key, language: language)
                guard let converted = RemoteText.remoteNotation(name: name, text: text) else { continue }
                exports.append("export NATIVEPIPE_TEXT_\(name)=\(SSHCommand.quote(converted))")
                exports.append("printf '%s\\n' \"$NATIVEPIPE_TEXT_\(name)\"")
                expected.append(converted)
            }
            XCTAssertEqual(try Self.shell(exports.joined(separator: "\n")), expected, language)

            let installing = try XCTUnwrap(RemoteText.remoteNotation(
                name: "INSTALL_INSTALLING",
                text: NPStrings.text("Installing the NativePipe compositor for %@ (%@)…", language: language)))
            let printed = try Self.shell("""
                set -eu
                \(say)
                export NATIVEPIPE_TEXT_INSTALL_INSTALLING=\(SSHCommand.quote(installing))
                say INSTALL_INSTALLING "english %s %s" aarch64 gnu 2>&1
                """)
            XCTAssertEqual(printed, [NPStrings.text("Installing the NativePipe compositor for %@ (%@)…",
                                                    arguments: ["aarch64", "gnu"], language: language)], language)
        }
    }

    private static func shell(_ script: String) throws -> [String] {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        process.arguments = ["-c", script]
        let output = Pipe()
        process.standardOutput = output
        process.standardError = output
        try process.run()
        let data = output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return String(decoding: data, as: UTF8.self).split(separator: "\n", omittingEmptySubsequences: false)
            .dropLast().map(String.init)
    }
}
