#!/usr/bin/env python3
"""Build pinned, private static AV1 dependencies; no host codec ABI at runtime."""
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
    "aom": ("https://aomedia.googlesource.com/aom", "33c921efad52392d6c7047a6768a1337cd106998"),
    "dav1d": ("https://code.videolan.org/videolan/dav1d.git", "191bdda98ec3c68137754dc97da1db34043d7cd4"),
    "libyuv": ("https://chromium.googlesource.com/libyuv/libyuv", "2dd4257364d39c38d79465c4ddc4b93137fe729b"),
}

def run(*args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)

def source(name):
    url, revision = DEPS[name]
    path = ROOT / ".build/codecs/src" / (name + "-" + revision)
    if not (path / ".complete").exists():
        path.mkdir(parents=True, exist_ok=True)
        if not (path / ".git").exists():
            run("git", "init", "-q", path)
        run("git", "-C", path, "fetch", "--depth=1", url, revision)
        run("git", "-C", path, "checkout", "--detach", "FETCH_HEAD")
        (path / ".complete").touch()
    return path

def build(name, target, arch, output, mac):
    src = source(name)
    work = ROOT / ".build/codecs/build" / target / name
    env = dict(os.environ)
    flags = "-O2 -fPIC" + (f" -arch {arch} -mmacosx-version-min=14.0" if mac else " -ffunction-sections -fdata-sections")
    env.update(CFLAGS=flags, CXXFLAGS=flags, LDFLAGS=flags)
    if name == "dav1d":
        args = ["meson", "setup", work, src, "--prefix=" + str(output),
                "--libdir=lib", "--default-library=static", "--buildtype=release",
                "-Denable_tools=false", "-Denable_tests=false", "-Dbitdepths=8"]
        if mac:
            cross = ROOT / ".build/codecs" / (target + ".ini")
            cpu = "aarch64" if arch == "arm64" else "x86_64"
            cross.write_text("[binaries]\nc = 'clang'\nar = 'ar'\nstrip = 'strip'\n"
                "[host_machine]\nsystem = 'darwin'\ncpu_family = '" + cpu + "'\n"
                "cpu = '" + cpu + "'\nendian = 'little'\n"
                "[built-in options]\nc_args = ['-arch', '" + arch + "', '-mmacosx-version-min=14.0']\n")
            args += ["--cross-file", cross]
        if (work / "build.ninja").exists():
            args += ["--reconfigure"]
        run(*args, env=env)
        run("ninja", "-C", work, "install", env=env)
    else:
        args = ["cmake", "--fresh", "-S", src, "-B", work, "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=" + str(output),
                "-DCMAKE_INSTALL_LIBDIR=lib", "-DBUILD_SHARED_LIBS=OFF",
                "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"]
        if mac:
            args += ["-DCMAKE_OSX_ARCHITECTURES=" + arch, "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0", "-DCMAKE_SYSTEM_NAME=Darwin", "-DCMAKE_SYSTEM_PROCESSOR=" + arch]
        if name == "aom":
            args += ["-DENABLE_DOCS=OFF", "-DENABLE_TESTS=OFF", "-DENABLE_EXAMPLES=OFF",
                     "-DENABLE_TOOLS=OFF", "-DCONFIG_AV1_DECODER=0", "-DCONFIG_AV1_HIGHBITDEPTH=0", "-DCONFIG_REALTIME_ONLY=1"]
        else:
            args += ["-DUNIT_TEST=OFF", "-DTEST=OFF", "-DJPEG_FOUND=OFF", "-DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON"]
        run(*args, env=env)
        run("cmake", "--build", work, "--target", "yuv" if name == "libyuv" else "all",
            "--parallel", str(min(os.cpu_count() or 2, 8)), env=env)
        if name == "libyuv":
            (output / "lib").mkdir(parents=True, exist_ok=True)
            shutil.copy2(work / "libyuv.a", output / "lib/libyuv.a")
            shutil.copytree(src / "include", output / "include", dirs_exist_ok=True)
        else:
            run("cmake", "--install", work, env=env)
    notices = output / "LICENSES" / name
    notices.mkdir(parents=True, exist_ok=True)
    for filename in ("LICENSE", "COPYING", "PATENTS", "AUTHORS"):
        if (src / filename).is_file():
            shutil.copy2(src / filename, notices / filename)
    (notices / "SOURCE.json").write_text(json.dumps(dict(zip(("url", "revision"), DEPS[name])), indent=2) + "\n")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=("macos", "aarch64-gnu", "aarch64-musl", "x86_64-gnu", "x86_64-musl"))
    args = parser.parse_args()
    mac = args.target == "macos"
    if mac and platform.system() != "Darwin":
        parser.error("macOS dependencies must be built on macOS")
    if not mac:
        arch, libc = args.target.split("-")
        if platform.system() != "Linux" or platform.machine().replace("arm64", "aarch64") != arch:
            parser.error("run inside a Linux environment with the selected architecture")
        probe = subprocess.run(["ldd", "--version"], capture_output=True, text=True)
        actual_libc = "musl" if "musl" in (probe.stdout + probe.stderr).lower() else "gnu"
        if actual_libc != libc:
            parser.error("run inside a Linux environment with the selected libc")
    base = ROOT / ".build/codecs"
    base.mkdir(parents=True, exist_ok=True)
    key = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    out = base / args.target
    marker = out / ".complete"
    libraries = ("libdav1d.a", "libyuv.a") if mac else ("libaom.a", "libyuv.a")
    header = out / "include/nativepipe_codec_build.h"
    if (marker.exists() and marker.read_text() == key and
        all((out / "lib" / name).is_file() for name in libraries) and (not mac or header.is_file())):
        return
    for arch in (("arm64", "x86_64") if mac else (args.target.split("-")[0],)):
        target = "macos-" + arch if mac else args.target
        prefix = base / target
        prefix.mkdir(parents=True, exist_ok=True)
        for name in (("dav1d", "libyuv") if mac else ("aom", "libyuv")):
            build(name, target, arch, prefix, mac)
    if mac:
        out.mkdir(parents=True, exist_ok=True)
        for directory in ("include", "LICENSES"):
            shutil.copytree(base / "macos-arm64" / directory, out / directory, dirs_exist_ok=True)
        (out / "lib").mkdir(exist_ok=True)
        for name in ("libdav1d.a", "libyuv.a"):
            run("lipo", "-create", base / "macos-arm64/lib" / name,
                base / "macos-x86_64/lib" / name, "-output", out / "lib" / name)
        # Clang tracks this include. Changing only a codec pin/recipe must
        # invalidate the bridge object and relink SwiftPM's cached executables.
        header.write_text('#define NP_CODEC_BUILD_ID "' + key + '"\n')
    marker.write_text(key)

if __name__ == "__main__":
    main()
