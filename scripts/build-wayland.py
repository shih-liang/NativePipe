#!/usr/bin/env python3
"""Build the pinned private Wayland library, scanner and protocol definitions."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEPS = {
    "wayland": ("https://github.com/wayland-mirror/wayland.git", "3e673a438b0a9749e3bdf5cac4befac86333024c"),  # 1.25.0
    "wayland-protocols": ("https://github.com/wayland-mirror/wayland-protocols.git", "ee78491a237eaff9389a0ccf8680521d074407d3"),  # 1.49
}


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=("aarch64-gnu", "aarch64-musl", "x86_64-gnu", "x86_64-musl"))
    args = parser.parse_args()
    arch, libc = args.target.split("-")
    probe = subprocess.run(["ldd", "--version"], capture_output=True, text=True)
    actual_libc = "musl" if "musl" in (probe.stdout + probe.stderr).lower() else "gnu"
    if platform.system() != "Linux" or platform.machine().replace("arm64", "aarch64") != arch or actual_libc != libc:
        parser.error("run inside a Linux environment with the selected architecture and libc")
    base = ROOT / ".build/wayland"
    output = base / args.target
    key = hashlib.sha256(Path(__file__).read_bytes() + str(output).encode()).hexdigest()
    marker = output / ".complete"
    required = ("lib/libwayland-server.a", "lib/libwayland-client.a", "bin/wayland-scanner",
                "lib/pkgconfig/wayland-server.pc", "lib/pkgconfig/wayland-client.pc", "lib/pkgconfig/wayland-scanner.pc",
                "share/pkgconfig/wayland-protocols.pc", "include/wayland-server-protocol.h", "include/wayland-client-protocol.h",
                "include/wayland-server-core.h", "include/wayland-client-core.h", "include/wayland-util.h", "include/wayland-version.h",
                "LICENSES/wayland/COPYING", "LICENSES/wayland/SOURCE.json",
                "LICENSES/wayland-protocols/COPYING", "LICENSES/wayland-protocols/SOURCE.json",
                "share/wayland-protocols/stable/xdg-shell/xdg-shell.xml",
                "share/wayland-protocols/stable/presentation-time/presentation-time.xml",
                "share/wayland-protocols/staging/fractional-scale/fractional-scale-v1.xml",
                "share/wayland-protocols/stable/viewporter/viewporter.xml",
                "share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml",
                "share/wayland-protocols/unstable/text-input/text-input-unstable-v3.xml",
                "share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml")
    if marker.is_file() and marker.read_text() == key and all((output / name).is_file() for name in required):
        return
    # A failed rebuild must not leave a cache marker for mixed dependency versions.
    marker.unlink(missing_ok=True)
    for name, (url, revision) in DEPS.items():
        source = base / "src" / (name + "-" + revision)
        source.mkdir(parents=True, exist_ok=True)
        if not (source / ".git").exists():
            run("git", "init", "-q", source)
        if not (source / ".complete").exists():
            run("git", "-C", source, "fetch", "--depth=1", url, revision)
            run("git", "-C", source, "checkout", "--detach", "FETCH_HEAD")
            actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
            if actual != revision:
                raise SystemExit("Wayland source revision did not match the pin")
            (source / ".complete").touch()
        work = base / "build" / args.target / (name + "-" + revision)
        env = dict(os.environ)
        env["PKG_CONFIG_PATH"] = str(output / "lib/pkgconfig") + os.pathsep + env.get("PKG_CONFIG_PATH", "")
        env["CFLAGS"] = "-O2 -fPIC -ffunction-sections -fdata-sections"
        options = ["-Dtests=false"]
        if name == "wayland":
            options += ["-Ddocumentation=false", "-Ddtd_validation=false", "-Dlibraries=true", "-Dscanner=true"]
        setup = ["meson", "setup", work, source, "--prefix=" + str(output), "--libdir=lib",
                 "--default-library=static", "--buildtype=release", *options]
        if (work / "build.ninja").exists():
            setup.append("--reconfigure")
        run(*setup, env=env)
        run("ninja", "-C", work, "-j", min(os.cpu_count() or 2, 8), "install", env=env)
        notice = output / "LICENSES" / name
        notice.mkdir(parents=True, exist_ok=True)
        for filename in ("COPYING", "LICENSE"):
            if (source / filename).is_file():
                shutil.copy2(source / filename, notice / filename)
        (notice / "SOURCE.json").write_text(json.dumps({"url": url, "revision": revision}, indent=2) + "\n")
    if not all((output / name).is_file() for name in required):
        raise SystemExit("Pinned Wayland installation is incomplete")
    marker.write_text(key)


if __name__ == "__main__":
    main()
