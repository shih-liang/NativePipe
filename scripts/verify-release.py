#!/usr/bin/env python3
"""Reject incomplete releases and unintended sidecar files before publishing."""
import hashlib
import json
import plistlib
from pathlib import Path
import sys
import tarfile

root = Path(sys.argv[1])
source = Path(__file__).resolve().parents[1]
license_files = json.loads((source / "LICENSES/source-inventory.json").read_text())["releaseLicenseFiles"]
archives = {"nativepipe-macos-universal.tar.gz"}
for arch in ("aarch64", "x86_64"):
    archives.add(f"nativepipe-vm-compositor-{arch}.tar.gz")
    archives.update(f"nativepipe-compositor-{arch}-{libc}.tar.gz" for libc in ("gnu", "musl"))
files = {p.name for p in root.iterdir()}
payloads = archives | {"install-compositor.sh"}
assert payloads | {"SHA256SUMS"} <= files, "A product, installer or checksum is missing"
assert files <= payloads | {"SHA256SUMS", "SHA256SUMS.sig", "nativepipe-ed25519-public-key.pem"}, "Unexpected release files"
checksums = {}
for line in (root / "SHA256SUMS").read_text().splitlines():
    digest, name = line.split("  ", 1)
    assert name not in checksums, "Duplicate checksum"
    checksums[name] = digest
assert set(checksums) == payloads, "Checksums must cover exactly the product archives and installer"
for name in sorted(payloads):
    with (root / name).open("rb") as handle:
        digest = hashlib.sha256()
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    assert digest.hexdigest() == checksums[name], f"Checksum mismatch: {name}"
    if name == "install-compositor.sh":
        assert (root / name).read_bytes() == (Path(__file__).parent / name).read_bytes(), "Installer differs from release source"
        continue
    with tarfile.open(root / name) as archive:
        members = {m.name.removeprefix("./"): m for m in archive.getmembers()}
        assert "VERSION" in members and members["VERSION"].isfile(), f"Missing release version in {name}"
        with archive.extractfile(members["VERSION"]) as handle:
            assert handle.read() == (source / "Sources/NativePipeStrings/Resources/VERSION").read_bytes(), f"Release version differs from source in {name}"
        if name.startswith("nativepipe-vm-"):
            arch = name.removeprefix("nativepipe-vm-compositor-").removesuffix(".tar.gz")
            required = [f"guest/compositor/dist/vmpipe-wayland-{arch}-{libc}" for libc in ("gnu", "musl")]
            required += [f"guest/session/dist/nativepipe-session-{arch}-{libc}" for libc in ("gnu", "musl")]
            required += [f"guest/session/dist/nativepipe-open-{arch}-{libc}" for libc in ("gnu", "musl")]
            assert not any(p.startswith("guest/compositor/dist/nativepipe-wayland-") for p in members), "Remote compositor duplicated in VM archive"
        elif name.startswith("nativepipe-compositor-"):
            required = ["nativepipe-wayland", "libexec/nativepipe-wayland", "libexec/np-open"]
        else:
            required = ["bin/nativepipe", "NativePipe.app/Contents/MacOS/nativepipe",
                        "NativePipe.app/Contents/Extensions/NativePipeFileSystemExtension.appex/Contents/MacOS/NativePipeFileSystemExtension"]
            metadata = "NativePipe.app/Contents/Info.plist"
            assert metadata in members and members[metadata].isfile(), f"Missing application metadata in {name}"
            with archive.extractfile(members[metadata]) as handle:
                info = plistlib.load(handle)
            version = (source / "Sources/NativePipeStrings/Resources/VERSION").read_text().strip()
            assert info.get("CFBundleIdentifier") == "com.nativepipe.cli" and info.get("CFBundleExecutable") == "nativepipe" and info.get("CFBundlePackageType") == "APPL", "Invalid NativePipe application identity"
            assert info.get("CFBundleShortVersionString") == version and info.get("CFBundleVersion") == version, "NativePipe application version differs from release"
            with archive.extractfile(members["bin/nativepipe"]) as handle:
                assert b'exec "$root/NativePipe.app/Contents/MacOS/nativepipe" "$@"' in handle.read(), "CLI entry does not execute its signed application"
            assert info.get("LSMinimumSystemVersion") == "14.0", "CLI deployment target must remain macOS 14"
            extension = "NativePipe.app/Contents/Extensions/NativePipeFileSystemExtension.appex/Contents/"
            module_metadata = extension + "Info.plist"
            assert module_metadata in members and members[module_metadata].isfile(), "Missing FSKit module metadata"
            with archive.extractfile(members[module_metadata]) as handle:
                module = plistlib.load(handle)
            assert (module.get("CFBundleIdentifier") == "com.nativepipe.cli.filesystem" and
                    module.get("CFBundleExecutable") == "NativePipeFileSystemExtension" and
                    module.get("CFBundlePackageType") == "XPC!"), "Invalid NativePipe FSKit module identity"
            assert module.get("CFBundleShortVersionString") == version and module.get("CFBundleVersion") == version, "FSKit module version differs from release"
            assert module.get("LSMinimumSystemVersion") == "27.0", "FSKit module must require macOS 27"
            attributes = module.get("EXAppExtensionAttributes", {})
            assert attributes.get("EXExtensionPointIdentifier") == "com.apple.fskit.fsmodule" and attributes.get("FSShortName") == "nativepipe", "Invalid FSKit module registration"
            assert attributes.get("FSSupportsPathURLs") is True and attributes.get("FSRequiresSecurityScopedPathURLResources") is True and attributes.get("FSSupportsBlockResources") is False, "Invalid FSKit resource policy"
            # This Linux-compatible archive check does not prove CMS or code
            # signature validity. The macOS packager checks the actual issued
            # profiles and signatures before it creates this archive.
            for profile in ("NativePipe.app/Contents/embedded.provisionprofile", extension + "embedded.provisionprofile"):
                assert profile in members and members[profile].isfile() and members[profile].size > 0, "Missing FSKit provisioning profile: " + profile
        for path in required:
            assert path in members and members[path].isfile() and members[path].mode & 0o111, f"Missing executable {path} in {name}"
        for path in license_files:
            assert path in members and members[path].isfile(), f"Missing project license file {path} in {name}"
            with archive.extractfile(members[path]) as handle:
                assert handle.read() == (source / path).read_bytes(), f"Project license file {path} differs from release source in {name}"
print("Release verified: 2 VM bundles, 4 remote bundles, 1 universal macOS CLI, installer, shared checksums")
