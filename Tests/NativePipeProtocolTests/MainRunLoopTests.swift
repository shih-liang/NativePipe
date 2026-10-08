import Darwin
import Foundation
import XCTest

final class MainRunLoopTests: XCTestCase {
    func testBackgroundDeliveryWakesIdleTrackingLoop() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let fixture = directory.appendingPathComponent("main.swift")
        try Self.fixture.write(to: fixture, atomically: true, encoding: .utf8)
        let executable = directory.appendingPathComponent("idle-tracking")
        // XCTest/AppKit can stop their shared main loop before our block runs.
        // Compile the production helper into a process with no ambient UI work.
        let compiled = try run(URL(fileURLWithPath: "/usr/bin/xcrun"), arguments: [
            "swiftc", "-swift-version", "5", "-module-cache-path", directory.appendingPathComponent("modules").path,
            root.appendingPathComponent("Sources/NativePipeProtocol/MainRunLoop.swift").path,
            fixture.path, "-o", executable.path
        ], directory: directory, timeout: 30)
        XCTAssertEqual(compiled.status, 0, compiled.output)
        guard compiled.status == 0 else { return }
        let checked = try run(executable, arguments: [], directory: directory, timeout: 6)
        XCTAssertEqual(checked.status, 0, checked.output)
        XCTAssertTrue(checked.output.hasPrefix("Idle tracking wake passed"), checked.output)
    }

    private func run(_ executable: URL, arguments: [String], directory: URL,
                     timeout: TimeInterval) throws -> (status: Int32, output: String) {
        let log = directory.appendingPathComponent(UUID().uuidString + ".log")
        _ = FileManager.default.createFile(atPath: log.path, contents: nil)
        let output = try FileHandle(forWritingTo: log)
        defer { try? output.close() }
        let process = Process(), ended = DispatchSemaphore(value: 0)
        process.executableURL = executable; process.arguments = arguments
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = output; process.standardError = output
        process.terminationHandler = { _ in ended.signal() }
        try process.run()
        if ended.wait(timeout: .now() + timeout) != .success {
            process.terminate()
            if ended.wait(timeout: .now() + 2) != .success {
                if process.isRunning { _ = kill(process.processIdentifier, SIGKILL) }
                _ = ended.wait(timeout: .now() + 2)
            }
            XCTFail("Isolated tracking check did not finish: \(executable.lastPathComponent)")
            return (-1, String(decoding: try Data(contentsOf: log), as: UTF8.self))
        }
        return (process.terminationStatus, String(decoding: try Data(contentsOf: log), as: UTF8.self))
    }

    private static let fixture = #"""
    import CoreFoundation
    import Darwin
    import Foundation

    MainActor.assumeIsolated {
        let loop = CFRunLoopGetMain()
        let mode = CFRunLoopMode(rawValue: "NSEventTrackingRunLoopMode" as CFString)
        var context = CFRunLoopSourceContext()
        let source = CFRunLoopSourceCreate(nil, 0, &context)!
        CFRunLoopAddSource(loop, source, mode)
        defer { CFRunLoopRemoveSource(loop, source, mode) }
        var delivered = false
        let workerReady = DispatchSemaphore(value: 0)
        let trackingWillWait = DispatchSemaphore(value: 0)
        let enqueued = DispatchSemaphore(value: 0)
        let observer = CFRunLoopObserverCreateWithHandler(nil,
            CFRunLoopActivity.beforeWaiting.rawValue, false, 0) { _, _ in trackingWillWait.signal() }!
        CFRunLoopAddObserver(loop, observer, mode)
        defer { CFRunLoopRemoveObserver(loop, observer, mode) }
        DispatchQueue.global(qos: .userInitiated).async {
            workerReady.signal()
            guard trackingWillWait.wait(timeout: .now() + 3) == .success else { return }
            let deadline = ProcessInfo.processInfo.systemUptime + 1
            while !CFRunLoopIsWaiting(loop), ProcessInfo.processInfo.systemUptime < deadline { sched_yield() }
            guard CFRunLoopIsWaiting(loop) else { return }
            MainRunLoop.perform {
                delivered = true
                CFRunLoopStop(loop)
            }
            enqueued.signal()
        }
        guard workerReady.wait(timeout: .now() + 2) == .success else {
            print("Background producer did not start"); exit(1)
        }
        let start = ProcessInfo.processInfo.systemUptime
        let result = CFRunLoopRunInMode(mode, 2, false)
        let elapsed = ProcessInfo.processInfo.systemUptime - start
        guard enqueued.wait(timeout: .now() + 2) == .success,
              delivered, result == .stopped, elapsed < 1 else {
            print("Idle tracking wake failed: delivered=\(delivered), result=\(result.rawValue), elapsed=\(elapsed)")
            exit(1)
        }
        print("Idle tracking wake passed: elapsed=\(elapsed)")
    }
    """#
}
