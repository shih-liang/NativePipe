import Foundation
import XCTest
@testable import NativePipeRemote

final class SSHAuthenticationTests: XCTestCase {
    func testTypedPasswordRequiresExplicitLoginPrompt() {
        for prompt in ["alice@host's password: ", "alice@2001:db8::1's password:", " configured-user@resolved-host's password: \n"] {
            XCTAssertTrue(SSHCredentialStore.isLoginPasswordPrompt(prompt), prompt)
        }
        for prompt in ["Password:", "One-time password:", "Verification code:", "OTP password:",
                       "Enter passphrase for key '/a/key':", "@host's password:", "alice@'s password:",
                       "Enter alice@host's password:", "alice@host's password: code"] {
            XCTAssertFalse(SSHCredentialStore.isLoginPasswordPrompt(prompt), prompt)
        }
        XCTAssertFalse(SSHCredentialStore.mayRemember("One-time password:"))
        XCTAssertFalse(SSHCredentialStore.mayRemember("Password:"))
        XCTAssertTrue(SSHCredentialStore.mayRemember("Enter passphrase for key '/a/key':"))
    }

    func testTypedPasswordWinsAndRejectionCannotTryLegacyOrChangedPrompt() throws {
        try withAttempt { directory in
            var typedReads = 0, promptReads = 0
            let first = SSHCredentialStore.cachedResponse(prompt: "alice@host's password: ", confirming: false,
                directory: directory.path, readLoginPassword: { typedReads += 1; return "fixture-login" },
                readPrompt: { _ in promptReads += 1; return "fixture-legacy" })
            XCTAssertEqual(first, "fixture-login")
            for prompt in ["alice@host's password: ", "alice@resolved-host's password:"] {
                XCTAssertNil(SSHCredentialStore.cachedResponse(prompt: prompt, confirming: false, directory: directory.path,
                    readLoginPassword: { typedReads += 1; return "fixture-login" },
                    readPrompt: { _ in promptReads += 1; return "fixture-legacy" }))
            }
            XCTAssertEqual(typedReads, 1)
            XCTAssertEqual(promptReads, 0)
            let markers = try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
            XCTAssertEqual(markers.count, 1)
            XCTAssertTrue(try Data(contentsOf: XCTUnwrap(markers.first)).isEmpty, "Retry marker must never contain a password")
        }
    }

    func testMissingTypedPasswordUsesExactLegacyPromptOnce() throws {
        try withAttempt { directory in
            let actualPrompt = "configured-user@resolved-host's password: "
            var requests: [String] = []
            let value = SSHCredentialStore.cachedResponse(prompt: actualPrompt, confirming: false, directory: directory.path,
                readLoginPassword: { nil }, readPrompt: { requests.append($0); return "fixture-legacy" })
            XCTAssertEqual(value, "fixture-legacy")
            XCTAssertEqual(requests, [actualPrompt], "Aliases must not be converted into guessed prompt text")
            XCTAssertNil(SSHCredentialStore.cachedResponse(prompt: actualPrompt, confirming: false, directory: directory.path,
                readLoginPassword: { XCTFail("Rejected legacy password must lead to UI"); return "fixture-login" },
                readPrompt: { _ in XCTFail("Legacy cache cannot replay twice"); return "fixture-legacy" }))
        }
    }

    func testPassphraseKeepsPromptCacheAndDoesNotConsumeLoginPassword() throws {
        try withAttempt { directory in
            for path in ["/a/key", "/b/key"] {
                let prompt = "Enter passphrase for key '\(path)':"
                var requests: [String] = []
                XCTAssertEqual(SSHCredentialStore.cachedResponse(prompt: prompt, confirming: false, directory: directory.path,
                    readLoginPassword: { XCTFail("Private-key prompts cannot use the login password"); return "fixture-login" },
                    readPrompt: { requests.append($0); return "fixture-passphrase" }), "fixture-passphrase")
                XCTAssertEqual(requests, [prompt])
                XCTAssertNil(SSHCredentialStore.cachedResponse(prompt: prompt, confirming: false, directory: directory.path,
                    readLoginPassword: { XCTFail(); return nil }, readPrompt: { _ in XCTFail(); return nil }))
            }
            XCTAssertEqual(SSHCredentialStore.cachedResponse(prompt: "alice@host's password:", confirming: false, directory: directory.path,
                readLoginPassword: { "fixture-login" }, readPrompt: { _ in XCTFail(); return nil }), "fixture-login")
        }
    }

    func testConfirmationChallengesAndMissingAttemptNeverReadSecrets() throws {
        try withAttempt { directory in
            for (prompt, confirming, path) in [
                ("alice@host's password:", true, directory.path),
                ("Password:", false, directory.path),
                ("One-time password:", false, directory.path),
                ("Verification code:", false, directory.path),
                ("alice@host's password:", false, nil),
                ("alice@host's password:", false, directory.appendingPathComponent("missing").path)
            ] {
                XCTAssertNil(SSHCredentialStore.cachedResponse(prompt: prompt, confirming: confirming, directory: path,
                    readLoginPassword: { XCTFail("Unexpected typed credential read for \(prompt)"); return nil },
                    readPrompt: { _ in XCTFail("Unexpected legacy credential read for \(prompt)"); return nil }))
            }
            XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: directory.path).isEmpty)
        }
    }

    func testKeychainFailureOpensPromptInsteadOfFallingBackToLegacy() throws {
        struct ReadFailure: Error {}
        try withAttempt { directory in
            XCTAssertThrowsError(try SSHCredentialStore.cachedResponse(prompt: "alice@host's password:", confirming: false,
                directory: directory.path, readLoginPassword: { throw ReadFailure() },
                readPrompt: { _ in XCTFail("Denied Keychain access must not silently try a different credential"); return nil }))
            XCTAssertNil(SSHCredentialStore.cachedResponse(prompt: "alice@host's password:", confirming: false, directory: directory.path,
                readLoginPassword: { XCTFail(); return nil }, readPrompt: { _ in XCTFail(); return nil }))
        }
    }

    func testLoginPasswordPreservesSpacesAndRejectsAskpassLineDelimiters() {
        for password in [" fixture ", "   ", "pässwörd🔑"] {
            XCTAssertNoThrow(try SSHCredentialStore.validateLoginPassword(password))
        }
        for password in ["", "a\0b", "a\rb", "a\nb", "a\r\nb"] {
            XCTAssertThrowsError(try SSHCredentialStore.validateLoginPassword(password))
        }
    }

    @MainActor func testPasswordPreferenceIsAcceptedByOpenSSHAndKeepsFallbackMethods() throws {
        let configuration = try sshConfiguration(arguments: SSHAuthentication.authenticationArguments(hasLoginPassword: true))
        XCTAssertTrue(configuration.contains("preferredauthentications password,gssapi-with-mic,hostbased,publickey,keyboard-interactive\n"))
        XCTAssertTrue(SSHAuthentication.authenticationArguments(hasLoginPassword: false).isEmpty)
    }

    @MainActor func testExplicitAuthenticationChoiceWinsOverCachedPasswordPreference() throws {
        for preference in ["publickey", "password,keyboard-interactive"] {
            for options in [
                ["-o", "PreferredAuthentications=" + preference],
                ["-opReFeRrEdAuThEnTiCaTiOnS=" + preference],
                ["-o", "preferredauthentications " + preference]
            ] {
                let injected = SSHAuthentication.authenticationArguments(hasLoginPassword: true, sshArguments: options)
                XCTAssertTrue(injected.isEmpty, "An explicit authentication mode must win even when a login password exists")
                let configuration = try sshConfiguration(arguments: injected + options)
                XCTAssertTrue(configuration.contains("preferredauthentications " + preference + "\n"), options.description)
            }
        }
    }

    @MainActor func testUnrelatedSSHOptionsKeepDefaultPasswordPreference() {
        for options in [["-o", "BatchMode=no"], ["-oIdentityAgent=none"],
                        ["-o", "IdentityFile=PreferredAuthentications=publickey"]] {
            XCTAssertEqual(SSHAuthentication.authenticationArguments(hasLoginPassword: true, sshArguments: options),
                           SSHAuthentication.authenticationArguments(hasLoginPassword: true))
        }
    }

    private func sshConfiguration(arguments: [String]) throws -> String {
        let child = Process(), output = Pipe(), errors = Pipe()
        child.executableURL = URL(fileURLWithPath: "/usr/bin/ssh")
        child.arguments = arguments + ["-F", "/dev/null", "-G", "alice@fixture.invalid"]
        child.standardOutput = output; child.standardError = errors
        try child.run()
        let configuration = String(decoding: output.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        child.waitUntilExit()
        XCTAssertEqual(child.terminationStatus, 0, String(decoding: errors.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self))
        return configuration
    }

    private func withAttempt(_ body: (URL) throws -> Void) throws {
        let directory = try SSHCredentialStore.makeAttemptDirectory(environment: [:])
        defer { try? FileManager.default.removeItem(at: directory) }
        try body(directory)
    }
}
