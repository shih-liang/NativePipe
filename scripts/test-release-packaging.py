#!/usr/bin/env python3
"""Exercise release assembly with disposable build artifacts, including failure."""
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile

with tempfile.TemporaryDirectory(prefix="nativepipe-release-check-") as folder:
    root = Path(folder)
    linux, macos = root / "linux", root / "macos"
    macos.mkdir()

    def archive(path, executables):
        with tarfile.open(path, "w:gz") as output:
            for name in executables + ["LICENSES/NOTICE"]:
                data = b"build fixture\n"
                item = tarfile.TarInfo(name)
                item.mode, item.size = 0o755, len(data)
                output.addfile(item, io.BytesIO(data))

    for arch in ("aarch64", "x86_64"):
        for libc in ("gnu", "musl"):
            artifact = linux / f"nativepipe-linux-{arch}-{libc}"
            for path in (f"guest/compositor/dist/vmpipe-wayland-{arch}-{libc}",
                         f"guest/session/dist/nativepipe-session-{arch}-{libc}",
                         f"guest/session/dist/nativepipe-align-blob-{arch}-{libc}.so",
                         f"guest/session/dist/nativepipe-vulkan-layer-{arch}-{libc}.so"):
                target = artifact / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"build fixture")  # Artifacts lose their executable mode.
            archive(artifact / f"nativepipe-compositor-{arch}-{libc}.tar.gz", ["nativepipe-wayland", "libexec/nativepipe-wayland"])
    archive(macos / "nativepipe-macos-universal.tar.gz", ["bin/nativepipe"])
    subprocess.run(["sh", "scripts/package-release.sh", str(linux), str(macos), str(root / "release")], check=True)
    subprocess.run(["python3", "scripts/verify-release.py", str(root / "release")], check=True)
    (root / "release/unnecessary-report.json").write_text("{}")
    rejected = subprocess.run(["python3", "scripts/verify-release.py", str(root / "release")], capture_output=True)
    assert rejected.returncode != 0, "Unexpected release files must fail validation"
    (macos / "nativepipe-macos-universal.tar.gz").unlink()
    rejected = subprocess.run(["sh", "scripts/package-release.sh", str(linux), str(macos), str(root / "incomplete")], capture_output=True)
    assert rejected.returncode != 0, "A missing CLI must prevent the release"
    print("PASS complete product set, restored executable modes, missing product rejection, unwanted file rejection")
