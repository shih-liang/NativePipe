#!/usr/bin/env python3
"""Exercise the production notification route with independent signed processes.

Usage: test-notification-routing.py <SwiftPM Products/Debug directory>
No notifications are posted and no authorization is requested.
"""
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import time

products = Path(sys.argv[1]).resolve()
module_map = products.parent.parent / 'Intermediates.noindex/GeneratedModuleMaps/CNativePipeFileRPC.modulemap'
helper = r'''
import AppKit
import Foundation
import NativePipeProtocol
import UserNotifications
@testable import NativePipeWindowing

@main struct NotificationFixture {
    @MainActor static func main() async throws {
        let args = Array(CommandLine.arguments.dropFirst())
        NSApplication.shared.setActivationPolicy(.prohibited)
        if args[0] == "identity" {
            let settings = await UNUserNotificationCenter.current().notificationSettings()
            print("bundle=\(Bundle.main.bundleIdentifier!) notificationSettings=\(settings.authorizationStatus.rawValue)")
            return
        }
        let root = URL(fileURLWithPath: args[1])
        if args[0] == "owner" {
            let name = args[2], output = root.appendingPathComponent(args[2] + ".received")
            let router = try GuestNotificationResponseRouter(directory: root) { identifier, action in
                guard identifier == "nativepipe.guest." + name, action == "open" || action == nil else { return false }
                let prior = (try? String(contentsOf: output, encoding: .utf8)) ?? ""
                try? (prior + "\(identifier):\(action ?? "dismiss")\n").write(to: output, atomically: true, encoding: .utf8)
                return true
            }
            try router.url.path.write(to: root.appendingPathComponent(name + ".ready"), atomically: true, encoding: .utf8)
            let deadline = ProcessInfo.processInfo.systemUptime + 20
            while !FileManager.default.fileExists(atPath: root.appendingPathComponent("stop").path) {
                guard ProcessInfo.processInfo.systemUptime < deadline else { throw CocoaError(.userCancelled) }
                try await Task.sleep(for: .milliseconds(10))
            }
            router.stop()
            print("stopped \(name)")
        } else {
            let path = try String(contentsOf: root.appendingPathComponent(args[2] + ".ready"), encoding: .utf8)
            let response = GuestNotificationResponseRouter.Response(identifier: args[3], action: args[4] == "dismiss" ? nil : args[4])
            if args[0] == "raw" {
                do {
                    let connection = try await SocketConnection.connect(to: URL(fileURLWithPath: path))
                    defer { connection.close() }
                    try await connection.write(WireFormat.frame(payload: JSONEncoder().encode(response)))
                    let reply = try? await connection.readExactly(1, deadline: .now() + .seconds(3))
                    print(reply == Data([1]) ? "accepted" : "rejected")
                } catch { print("rejected") }
            } else {
                let accepted = await GuestNotificationResponseRouter.forward(response, path: path,
                    permittedDirectories: [root.appendingPathComponent(".n", isDirectory: true)])
                print(accepted ? "accepted" : "rejected")
            }
        }
    }
}
'''

def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=30).stdout.strip()

with tempfile.TemporaryDirectory(prefix='npr-', dir='/tmp') as temporary:
    root = Path(temporary)
    source = root / 'main.swift'
    source.write_text(helper)
    app = root / 'NotificationFixture.app'
    executable = app / 'Contents/MacOS/probe'
    executable.parent.mkdir(parents=True)
    run('xcrun', 'swiftc', '-parse-as-library', '-module-cache-path', str(root / 'modules'),
        '-I', str(products), '-Xcc', '-fmodule-map-file=' + str(module_map),
        '-L', str(products), '-lNativePipeWindowing', '-lNativePipeProtocol',
        str(products / 'NativePipeStrings.o'), str(products / 'CNativePipeFileRPC.o'),
        str(source), '-o', str(executable))
    metadata = {'CFBundleIdentifier': 'com.nativepipe.notification-fixture', 'CFBundleExecutable': 'probe',
                'CFBundlePackageType': 'APPL', 'CFBundleVersion': '1', 'LSUIElement': True}
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(metadata))
    run('codesign', '--force', '--sign', '-', str(app))
    run('codesign', '--verify', '--strict', str(app))
    identity = run(str(executable), 'identity')
    assert identity.startswith('bundle=com.nativepipe.notification-fixture notificationSettings='), identity
    owners = [subprocess.Popen([str(executable), 'owner', str(root), name], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True) for name in ('A', 'B')]
    try:
        deadline = time.monotonic() + 10
        while not all((root / (name + '.ready')).exists() for name in ('A', 'B')):
            assert all(owner.poll() is None for owner in owners), 'Owner unexpectedly exited'
            assert time.monotonic() < deadline, 'Owner did not start'
            time.sleep(.01)
        for owner in ('A', 'B'):
            assert run(str(executable), 'forward', str(root), owner, 'nativepipe.guest.' + owner, 'open') == 'accepted'
            other = 'B' if owner == 'A' else 'A'
            assert run(str(executable), 'forward', str(root), owner, 'nativepipe.guest.' + other, 'open') == 'rejected'
            assert run(str(executable), 'forward', str(root), owner, 'nativepipe.guest.' + owner, 'unknown') == 'rejected'
            assert run(str(executable), 'forward', str(root), owner, 'nativepipe.guest.old', 'open') == 'rejected'
            assert run(str(executable), 'forward', str(root), owner, 'nativepipe.guest.' + owner, 'dismiss') == 'accepted'
        # Another bundle cannot impersonate the app's response delegate, even
        # when running as the same user and knowing the endpoint and action.
        unrelated = root / 'Unrelated.app'
        import shutil
        shutil.copytree(app, unrelated)
        metadata['CFBundleIdentifier'] = 'com.nativepipe.unrelated-fixture'
        (unrelated / 'Contents/Info.plist').write_bytes(plistlib.dumps(metadata))
        run('codesign', '--force', '--sign', '-', str(unrelated))
        assert run(str(unrelated / 'Contents/MacOS/probe'), 'raw', str(root), 'B', 'nativepipe.guest.B', 'open') == 'rejected'
        for owner in ('A', 'B'):
            assert (root / (owner + '.received')).read_text().splitlines() == [
                'nativepipe.guest.' + owner + ':open', 'nativepipe.guest.' + owner + ':dismiss']
        endpoints = [(root / (name + '.ready')).read_text() for name in ('A', 'B')]
        (root / 'stop').touch()
        for owner in owners:
            stdout, stderr = owner.communicate(timeout=5)
            assert owner.returncode == 0, stderr
        assert not any(Path(endpoint).exists() for endpoint in endpoints), 'Stopped session kept accepting actions'
    finally:
        (root / 'stop').touch()
        for owner in owners:
            if owner.poll() is None:
                owner.terminate()
                owner.wait(timeout=5)
    print('Notification routing verified: signed app identity, two independent owners, action/dismiss, stale/unknown/wrong-bundle rejection and endpoint cleanup')
