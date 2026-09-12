#!/usr/bin/env python3
"""Reject incomplete releases and unintended sidecar files before publishing."""
import hashlib
from pathlib import Path
import sys
import tarfile

root = Path(sys.argv[1])
archives = {"nativepipe-macos-universal.tar.gz"}
for arch in ("aarch64", "x86_64"):
    archives.add(f"nativepipe-vm-compositor-{arch}.tar.gz")
    archives.update(f"nativepipe-compositor-{arch}-{libc}.tar.gz" for libc in ("gnu", "musl"))
files = {p.name for p in root.iterdir()}
assert archives | {"SHA256SUMS"} <= files, "A product or checksum is missing"
assert files <= archives | {"SHA256SUMS", "SHA256SUMS.sig", "nativepipe-ed25519-public-key.pem"}, "Unexpected release files"
checksums = {}
for line in (root / "SHA256SUMS").read_text().splitlines():
    digest, name = line.split("  ", 1)
    assert name not in checksums, "Duplicate checksum"
    checksums[name] = digest
assert set(checksums) == archives, "Checksums must cover exactly the product archives"
for name in sorted(archives):
    with (root / name).open("rb") as handle:
        digest = hashlib.sha256()
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    assert digest.hexdigest() == checksums[name], f"Checksum mismatch: {name}"
    with tarfile.open(root / name) as archive:
        members = {m.name.removeprefix("./"): m for m in archive.getmembers()}
        if name.startswith("nativepipe-vm-"):
            arch = name.removeprefix("nativepipe-vm-compositor-").removesuffix(".tar.gz")
            required = [f"guest/compositor/dist/vmpipe-wayland-{arch}-{libc}" for libc in ("gnu", "musl")]
            required += [f"guest/session/dist/nativepipe-session-{arch}-{libc}" for libc in ("gnu", "musl")]
            assert not any(p.startswith("guest/compositor/dist/nativepipe-wayland-") for p in members), "Remote compositor duplicated in VM archive"
        elif name.startswith("nativepipe-compositor-"):
            required = ["nativepipe-wayland", "libexec/nativepipe-wayland"]
        else:
            required = ["bin/nativepipe"]
        for path in required:
            assert path in members and members[path].isfile() and members[path].mode & 0o111, f"Missing executable {path} in {name}"
        assert any(p.startswith("LICENSES/") for p in members), f"Missing notices in {name}"
print("Release verified: 2 VM bundles, 4 remote bundles, 1 universal macOS CLI, shared checksums")
