#!/usr/bin/env python3
"""Exercise release assembly with disposable build artifacts, including failure."""
import io
import hashlib
import json
import plistlib
from pathlib import Path
import subprocess
import tarfile
import tempfile

source = Path(__file__).resolve().parents[1]
license_files = json.loads((source / "LICENSES/source-inventory.json").read_text())["releaseLicenseFiles"]

with tempfile.TemporaryDirectory(prefix="nativepipe-release-check-") as folder:
    root = Path(folder)
    linux, macos = root / "linux", root / "macos"
    macos.mkdir()

    def archive(path, executables):
        with tarfile.open(path, "w:gz") as output:
            extra = ["NativePipe.app/Contents/Info.plist"] if path.name == "nativepipe-macos-universal.tar.gz" else []
            for name in executables + license_files + ["VERSION"] + extra:
                if name == "VERSION":
                    data = (source / "Sources/NativePipeStrings/Resources/VERSION").read_bytes()
                elif name == "NativePipe.app/Contents/Info.plist":
                    version = (source / "Sources/NativePipeStrings/Resources/VERSION").read_text().strip()
                    data = plistlib.dumps({"CFBundleExecutable": "nativepipe", "CFBundleIdentifier": "com.nativepipe.cli", "CFBundlePackageType": "APPL", "CFBundleShortVersionString": version, "CFBundleVersion": version})
                elif name == "bin/nativepipe":
                    data = b'#!/bin/sh\nroot=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)\nexec "$root/NativePipe.app/Contents/MacOS/nativepipe" "$@"\n'
                else:
                    data = (source / name).read_bytes() if name in license_files else b"build fixture\n"
                item = tarfile.TarInfo(name)
                item.mode, item.size = (0o755 if name in executables else 0o644), len(data)
                output.addfile(item, io.BytesIO(data))

    for arch in ("aarch64", "x86_64"):
        for libc in ("gnu", "musl"):
            artifact = linux / f"nativepipe-linux-{arch}-{libc}"
            for path in (f"guest/compositor/dist/vmpipe-wayland-{arch}-{libc}",
                         f"guest/session/dist/nativepipe-session-{arch}-{libc}",
                         f"guest/session/dist/nativepipe-open-{arch}-{libc}",
                         f"guest/session/dist/nativepipe-align-blob-{arch}-{libc}.so",
                         f"guest/session/dist/nativepipe-vulkan-layer-{arch}-{libc}.so"):
                target = artifact / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"build fixture")  # Artifacts lose their executable mode.
            archive(artifact / f"nativepipe-compositor-{arch}-{libc}.tar.gz", ["nativepipe-wayland", "libexec/nativepipe-wayland", "libexec/np-open"])
    archive(macos / "nativepipe-macos-universal.tar.gz", ["bin/nativepipe", "NativePipe.app/Contents/MacOS/nativepipe"])
    subprocess.run(["sh", "scripts/package-release.sh", str(linux), str(macos), str(root / "release")], check=True)
    subprocess.run(["python3", "scripts/verify-release.py", str(root / "release")], check=True)

    # Recompute checksums after changing notices, so these failures exercise
    # license validation rather than archive digest validation.
    sums = root / "release/SHA256SUMS"
    original_sums = sums.read_bytes()
    for name in ("nativepipe-vm-compositor-aarch64.tar.gz",
                 "nativepipe-compositor-aarch64-gnu.tar.gz",
                 "nativepipe-macos-universal.tar.gz"):
        bundle = root / "release" / name
        original_bundle = bundle.read_bytes()
        with tarfile.open(bundle) as content:
            entries = [(member, content.extractfile(member).read() if member.isfile() else b"")
                       for member in content.getmembers()]
        required_helpers = ["libexec/np-open"] if name.startswith("nativepipe-compositor-") else []
        required_metadata = []
        if name == "nativepipe-macos-universal.tar.gz":
            required_helpers = ["NativePipe.app/Contents/MacOS/nativepipe"]
            required_metadata = ["NativePipe.app/Contents/Info.plist"]
        for missing in license_files + ["VERSION"] + required_helpers + required_metadata:
            with tarfile.open(bundle, "w:gz") as output:
                for member, data in entries:
                    if member.name.removeprefix("./") != missing:
                        output.addfile(member, io.BytesIO(data) if member.isfile() else None)
            digest = hashlib.sha256(bundle.read_bytes()).hexdigest()
            sums.write_text("".join(
                f"{digest}  {name}\n" if line.split("  ", 1)[1] == name else line + "\n"
                for line in original_sums.decode().splitlines()))
            rejected = subprocess.run(["python3", "scripts/verify-release.py", str(root / "release")], capture_output=True)
            expected = ("Missing release version" if missing == "VERSION" else
                        "Missing application metadata" if missing in required_metadata else
                        f"Missing executable {missing}" if missing in required_helpers else
                        f"Missing project license file {missing}")
            assert rejected.returncode != 0 and expected.encode() in rejected.stderr, (name, missing, rejected.stderr)

        with tarfile.open(bundle, "w:gz") as output:
            for member, data in entries:
                if member.name.removeprefix("./") == "LICENSE":
                    data = b"Truncated license text\n"
                    member.size = len(data)
                output.addfile(member, io.BytesIO(data) if member.isfile() else None)
        digest = hashlib.sha256(bundle.read_bytes()).hexdigest()
        sums.write_text("".join(
            f"{digest}  {name}\n" if line.split("  ", 1)[1] == name else line + "\n"
            for line in original_sums.decode().splitlines()))
        rejected = subprocess.run(["python3", "scripts/verify-release.py", str(root / "release")], capture_output=True)
        assert rejected.returncode != 0 and b"Project license file LICENSE differs" in rejected.stderr, (name, rejected.stderr)
        bundle.write_bytes(original_bundle)
        sums.write_bytes(original_sums)

    installer = root / "release/install-compositor.sh"
    original = installer.read_bytes()
    installer.write_bytes(original + b"\n# changed after packaging\n")
    rejected = subprocess.run(["python3", "scripts/verify-release.py", str(root / "release")], capture_output=True)
    assert rejected.returncode != 0, "A changed installer must fail validation"
    installer.unlink()
    rejected = subprocess.run(["python3", "scripts/verify-release.py", str(root / "release")], capture_output=True)
    assert rejected.returncode != 0, "A missing installer must prevent the release"
    installer.write_bytes(original)
    (root / "release/unnecessary-report.json").write_text("{}")
    rejected = subprocess.run(["python3", "scripts/verify-release.py", str(root / "release")], capture_output=True)
    assert rejected.returncode != 0, "Unexpected release files must fail validation"
    (macos / "nativepipe-macos-universal.tar.gz").unlink()
    rejected = subprocess.run(["sh", "scripts/package-release.sh", str(linux), str(macos), str(root / "incomplete")], capture_output=True)
    assert rejected.returncode != 0, "A missing CLI must prevent the release"
    print("PASS complete product set, release versions, restored executable modes, complete licenses for all products, missing/changed license rejection, installer integrity, missing product rejection, unwanted file rejection")
