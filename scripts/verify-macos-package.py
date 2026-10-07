#!/usr/bin/env python3
"""Verify the signed standalone command and its real native FSKit extension."""
from pathlib import Path
import plistlib
import subprocess
import sys


def require(condition, message):
    if not condition:
        raise SystemExit("NativePipe package: " + message)


def plist(path):
    require(path.is_file(), "missing metadata: " + str(path))
    return plistlib.loads(path.read_bytes())


def check_resources(bundle, languages, version):
    resources = bundle / "Contents/Resources/NativePipe_NativePipeStrings.bundle"
    require(resources.is_dir(), "missing NativePipe translations in " + bundle.name)
    versions = list(resources.rglob("VERSION"))
    require(len(versions) == 1 and versions[0].read_text().strip() == version,
            "resource version differs from command version in " + bundle.name)
    for language in languages:
        paths = [resources / (language + ".lproj") / "Localizable.strings",
                 resources / (language.lower() + ".lproj") / "Localizable.strings",
                 resources / "Contents/Resources" / (language + ".lproj") / "Localizable.strings",
                 resources / "Contents/Resources" / (language.lower() + ".lproj") / "Localizable.strings"]
        require(any(path.is_file() and path.stat().st_size for path in paths),
                "missing language " + language + " in " + bundle.name)


def verify(app):
    host = plist(app / "Contents/Info.plist")
    require(host.get("CFBundleIdentifier") == "com.nativepipe.cli", "incorrect command App ID")
    require(host.get("CFBundleExecutable") == "nativepipe", "incorrect command executable")
    require(host.get("CFBundlePackageType") == "APPL", "incorrect command bundle type")
    require(host.get("LSMinimumSystemVersion") == "14.0", "command deployment target must remain macOS 14")
    version = host.get("CFBundleShortVersionString")
    require(isinstance(version, str) and host.get("CFBundleVersion") == version, "invalid command version")
    languages = host.get("CFBundleLocalizations", [])
    source_languages = sorted(path.stem for path in (Path(__file__).resolve().parents[1] / "Sources/NativePipeStrings/Resources").glob("*.lproj"))
    require(sorted(languages) == source_languages, "command language inventory differs from source")
    extension = app / "Contents/Extensions/NativePipeFileSystemExtension.appex"
    info = plist(extension / "Contents/Info.plist")
    require(info.get("CFBundleIdentifier") == "com.nativepipe.cli.filesystem", "incorrect module App ID")
    require(info.get("CFBundleExecutable") == "NativePipeFileSystemExtension", "incorrect module executable")
    require(info.get("CFBundlePackageType") == "XPC!", "incorrect extension bundle type")
    require(info.get("LSMinimumSystemVersion") == "27.0", "module requires macOS 27")
    require(info.get("CFBundleShortVersionString") == version and info.get("CFBundleVersion") == version,
            "module version differs from command version")
    require(info.get("CFBundleLocalizations") == languages, "module language inventory differs from command")
    attributes = info.get("EXAppExtensionAttributes", {})
    require(attributes.get("EXExtensionPointIdentifier") == "com.apple.fskit.fsmodule", "incorrect FSKit extension point")
    require(attributes.get("FSShortName") == "nativepipe", "incorrect filesystem type")
    require(attributes.get("FSSupportsPathURLs") is True and
            attributes.get("FSRequiresSecurityScopedPathURLResources") is True and
            attributes.get("FSSupportsBlockResources") is False, "incorrect FSKit resource policy")
    for language in languages:
        name = extension / "Contents/Resources" / (language + ".lproj") / "InfoPlist.strings"
        require(name.is_file(), "missing module name localization: " + language)
        subprocess.run(["plutil", "-lint", str(name)], check=True, capture_output=True)
    for bundle, capability in ((extension, "fsmodule"), (app, "mount")):
        binary = bundle / "Contents/MacOS" / (info["CFBundleExecutable"] if bundle == extension else "nativepipe")
        require(binary.is_file(), "missing executable in " + bundle.name)
        subprocess.run(["lipo", str(binary), "-verify_arch", "arm64", "x86_64"], check=True, capture_output=True)
        check_resources(bundle, languages, version)
        subprocess.run(["codesign", "--verify", "--strict", str(bundle)], check=True, capture_output=True)
        entitlements = plistlib.loads(subprocess.run(["codesign", "-d", "--entitlements", ":-", str(bundle)],
                                                    check=True, capture_output=True).stdout)
        require(entitlements.get("com.apple.developer.fskit." + capability) is True,
                "missing FSKit entitlement in " + bundle.name)
        if bundle == extension:
            require(entitlements.get("com.apple.security.app-sandbox") is True and
                    entitlements.get("com.apple.security.network.client") is True, "module sandbox/socket entitlement missing")
            for key in ("com.apple.security.application-groups", "com.apple.security.virtualization",
                        "com.apple.developer.fskit.mount", "com.apple.security.network.server"):
                require(key not in entitlements, "unnecessary module entitlement: " + key)
        load_commands = subprocess.run(["otool", "-l", str(binary)], check=True, capture_output=True, text=True).stdout
        require("/FSKit.framework/" in load_commands, "FSKit linkage missing in " + bundle.name)
        if bundle == app:
            blocks = load_commands.split("Load command ")
            require(any("LC_LOAD_WEAK_DYLIB" in block and "/FSKit.framework/" in block for block in blocks),
                    "command must weak-link FSKit to preserve macOS 14 display support")
    subprocess.run([sys.executable, str(Path(__file__).with_name("verify-fskit-profiles.py")), "--app", str(app)], check=True)
    print("Signed NativePipe command, FSKit module, native resources and issued profiles verified")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify-macos-package.py NativePipe.app")
    verify(Path(sys.argv[1]))
