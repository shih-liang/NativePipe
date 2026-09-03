#!/usr/bin/env python3
"""Create deterministic license notices for a locked xwayland-satellite build."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import textwrap
from typing import Any
import xml.etree.ElementTree as ET


class NoticeError(ValueError):
    pass


OPEN_SANS_RELATIVE_PATH = Path("OpenSans-Regular.ttf")
OPEN_SANS_SHA256 = "33e93bec67d91c396876db50694213802a39e43a911bb5c22322f0dbdf4d5e43"
OPEN_SANS_LICENSE_PATH = Path(__file__).resolve().parents[1] / "LICENSES/OFL-1.1.txt"
WL_DRM_XML_RELATIVE_PATH = Path("wl_drm/src/drm.xml")
RUST_COPYRIGHT_FALLBACKS = {
    "1.89.0": (
        Path(__file__).resolve().parents[1] / "LICENSES/Rust-1.89.0-COPYRIGHT.txt",
        "172020dbfd5b53a226dfde77616190a48dcff519b0bc0e6deb91a8450782c4af",
    ),
}


def run(command: list[str], *, cwd: Path) -> str:
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise NoticeError(f"cannot execute {command[0]}: {error}") from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise NoticeError(f"{' '.join(command)} failed: {detail}")
    return completed.stdout


def read_rust_version(rustc: str, source_dir: Path) -> tuple[str, str]:
    verbose = run([rustc, "--version", "--verbose"], cwd=source_dir)
    release = ""
    host = ""
    for line in verbose.splitlines():
        if line.startswith("release: "):
            release = line.removeprefix("release: ").strip()
        elif line.startswith("host: "):
            host = line.removeprefix("host: ").strip()
    if not release or not host:
        raise NoticeError("rustc --version --verbose omitted release or host")
    return release, host


def dependency_closure(metadata: dict[str, Any]) -> list[dict[str, Any]]:
    packages = metadata.get("packages")
    resolve = metadata.get("resolve")
    if not isinstance(packages, list) or not isinstance(resolve, dict):
        raise NoticeError("cargo metadata omitted packages or resolve graph")
    by_id = {package.get("id"): package for package in packages if isinstance(package, dict)}
    roots = [
        package
        for package in packages
        if isinstance(package, dict)
        and package.get("name") == "xwayland-satellite"
        and package.get("source") is None
    ]
    if len(roots) != 1:
        raise NoticeError("cargo metadata must contain one workspace xwayland-satellite package")
    nodes = {
        node.get("id"): node
        for node in resolve.get("nodes", [])
        if isinstance(node, dict)
    }
    pending = [roots[0]["id"]]
    visited: set[str] = set()
    while pending:
        package_id = pending.pop()
        if package_id in visited:
            continue
        if package_id not in by_id or package_id not in nodes:
            raise NoticeError(f"cargo resolve graph references unknown package {package_id!r}")
        visited.add(package_id)
        for dependency in nodes[package_id].get("deps", []):
            if not isinstance(dependency, dict):
                raise NoticeError("cargo dependency edge is not an object")
            kinds = dependency.get("dep_kinds")
            if not isinstance(kinds, list):
                raise NoticeError("cargo dependency edge omitted dep_kinds")
            if any(isinstance(kind, dict) and kind.get("kind") != "dev" for kind in kinds):
                pending.append(dependency.get("pkg"))
    return sorted(
        (by_id[package_id] for package_id in visited),
        key=lambda package: (str(package.get("name")), str(package.get("version"))),
    )


def package_license_files(package: dict[str, Any], source_root: Path) -> list[Path]:
    manifest = Path(str(package.get("manifest_path", ""))).resolve()
    try:
        manifest.relative_to(source_root)
        workspace_package = True
    except ValueError:
        workspace_package = False
    package_root = manifest.parent
    candidates: list[Path] = []
    license_file = package.get("license_file")
    if isinstance(license_file, str) and license_file:
        candidate = Path(license_file)
        candidates.append(candidate if candidate.is_absolute() else package_root / candidate)
    for candidate in package_root.iterdir():
        upper = candidate.name.upper()
        if candidate.is_file() and upper.startswith(
            ("LICENSE", "COPYING", "NOTICE", "COPYRIGHT", "UNLICENSE")
        ):
            candidates.append(candidate)
    if workspace_package:
        candidates.append(source_root / "LICENSE")
    unique: dict[Path, None] = {}
    for candidate in candidates:
        try:
            resolved = candidate.resolve(strict=True)
        except OSError as error:
            raise NoticeError(f"cannot resolve license file {candidate}: {error}") from error
        if not resolved.is_file():
            raise NoticeError(f"license path is not a regular file: {candidate}")
        unique[resolved] = None
    return sorted(unique, key=lambda path: path.name)


def rust_license_files(
    rustc: str, source_dir: Path, expected_rust_version: str
) -> list[Path]:
    sysroot = Path(run([rustc, "--print", "sysroot"], cwd=source_dir).strip()).resolve()
    if not sysroot.is_dir():
        raise NoticeError(f"Rust sysroot is not a directory: {sysroot}")
    required = {
        "COPYRIGHT": None,
        "LICENSE-APACHE": None,
        "LICENSE-MIT": None,
    }
    candidates = sorted(
        sysroot.rglob("*"), key=lambda path: (len(path.relative_to(sysroot).parts), path.as_posix())
    )
    for candidate in candidates:
        if candidate.is_file() and candidate.name in required and required[candidate.name] is None:
            required[candidate.name] = candidate
    if required["COPYRIGHT"] is None and expected_rust_version in RUST_COPYRIGHT_FALLBACKS:
        fallback, expected_digest = RUST_COPYRIGHT_FALLBACKS[expected_rust_version]
        try:
            digest = hashlib.sha256(fallback.read_bytes()).hexdigest()
        except OSError as error:
            raise NoticeError(f"cannot read pinned Rust COPYRIGHT fallback: {error}") from error
        if digest != expected_digest:
            raise NoticeError("pinned Rust COPYRIGHT fallback failed its digest check")
        required["COPYRIGHT"] = fallback
    missing = [name for name, path in required.items() if path is None]
    if missing:
        raise NoticeError(
            "Rust toolchain omits required standard-library notices: " + ", ".join(missing)
        )
    return [required[name] for name in sorted(required)]  # type: ignore[misc]


def normalized_text(path: Path) -> str:
    try:
        value = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise NoticeError(f"cannot read UTF-8 license text {path}: {error}") from error
    return value.replace("\r\n", "\n").replace("\r", "\n").rstrip() + "\n"


def wl_drm_license_text(source_dir: Path) -> str:
    path = source_dir / WL_DRM_XML_RELATIVE_PATH
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        raise NoticeError(f"cannot read wl_drm protocol notice {path}: {error}") from error
    notice = root.findtext("copyright")
    if not notice or "Permission to use, copy, modify" not in notice:
        raise NoticeError("wl_drm protocol omits its embedded MIT-style permission notice")
    # The pinned upstream XML contains one literal ``\\n`` typo. Preserve the
    # words while rendering the notice as ordinary human-readable text.
    return textwrap.dedent(notice).replace("\\n", "\n").strip() + "\n"


def build_notice(
    source_dir: Path,
    cargo: str,
    rustc: str,
    expected_rust_version: str,
) -> str:
    source_dir = source_dir.resolve()
    release, host = read_rust_version(rustc, source_dir)
    if release != expected_rust_version:
        raise NoticeError(
            f"Rust release {release!r} does not match audited release {expected_rust_version!r}"
        )
    metadata_text = run(
        [
            cargo,
            "metadata",
            "--locked",
            "--format-version",
            "1",
            "--filter-platform",
            host,
        ],
        cwd=source_dir,
    )
    try:
        metadata = json.loads(metadata_text)
    except json.JSONDecodeError as error:
        raise NoticeError(f"cargo metadata returned invalid JSON: {error}") from error
    packages = dependency_closure(metadata)
    if not packages:
        raise NoticeError("cargo dependency closure is empty")

    inventories: list[str] = []
    text_users: dict[str, set[str]] = {}
    texts: dict[str, str] = {}
    root_license = source_dir / "LICENSE"
    if not root_license.is_file():
        raise NoticeError("xwayland-satellite source omits repository LICENSE")
    for package in packages:
        name = str(package.get("name", ""))
        version = str(package.get("version", ""))
        source = package.get("source")
        expression = package.get("license")
        if source is None:
            expression = expression or "MPL-2.0"
            if name == "wl_drm":
                expression = f"{expression} AND MIT"
            provenance = "xwayland-satellite workspace"
        else:
            if not isinstance(expression, str) or not expression.strip():
                raise NoticeError(f"registry package {name} {version} has no license expression")
            if "NOASSERTION" in expression.upper() or expression.upper() == "NONE":
                raise NoticeError(f"registry package {name} {version} has unresolved license")
            provenance = str(source)
        files = package_license_files(package, source_dir)
        file_digests: list[str] = []
        for license_path in files:
            contents = normalized_text(license_path)
            digest = hashlib.sha256(contents.encode("utf-8")).hexdigest()
            texts.setdefault(digest, contents)
            text_users.setdefault(digest, set()).add(f"{name} {version} ({license_path.name})")
            file_digests.append(digest)
        inventories.append(
            f"- {name} {version} | {expression} | {provenance} | "
            + (", ".join(sorted(set(file_digests))) if file_digests else "no package-local text")
        )

    for rust_path in rust_license_files(rustc, source_dir, expected_rust_version):
        contents = normalized_text(rust_path)
        digest = hashlib.sha256(contents.encode("utf-8")).hexdigest()
        texts.setdefault(digest, contents)
        text_users.setdefault(digest, set()).add(
            f"Rust standard library {expected_rust_version} ({rust_path.name})"
        )

    open_sans = source_dir / OPEN_SANS_RELATIVE_PATH
    try:
        open_sans_digest = hashlib.sha256(open_sans.read_bytes()).hexdigest()
    except OSError as error:
        raise NoticeError(f"cannot read embedded Open Sans font: {error}") from error
    if open_sans_digest != OPEN_SANS_SHA256:
        raise NoticeError(
            "embedded Open Sans font differs from the audited pinned asset: "
            f"{open_sans_digest}"
        )
    ofl_text = normalized_text(OPEN_SANS_LICENSE_PATH)
    if "SIL OPEN FONT LICENSE Version 1.1" not in ofl_text:
        raise NoticeError("bundled Open Sans OFL-1.1 text is invalid")
    ofl_digest = hashlib.sha256(ofl_text.encode("utf-8")).hexdigest()
    texts.setdefault(ofl_digest, ofl_text)
    inventories.append(
        "- OpenSans-Regular.ttf | OFL-1.1 | Open Sans 3.003; copyright 2020 "
        f"The Open Sans Project Authors | {open_sans_digest}"
    )
    text_users.setdefault(ofl_digest, set()).add(
        "OpenSans-Regular.ttf (OFL-1.1; embedded asset)"
    )

    wl_drm_notice = wl_drm_license_text(source_dir)
    wl_drm_digest = hashlib.sha256(wl_drm_notice.encode("utf-8")).hexdigest()
    texts.setdefault(wl_drm_digest, wl_drm_notice)
    text_users.setdefault(wl_drm_digest, set()).add(
        "wl_drm/src/drm.xml (embedded MIT-style protocol notice)"
    )

    upstream_commit = run(["git", "rev-parse", "HEAD"], cwd=source_dir).strip()
    lines = [
        "LightHouse xwayland-satellite third-party notices",
        "",
        f"Upstream commit: {upstream_commit}",
        f"Rust standard library: {expected_rust_version}",
        f"Rust host target: {host}",
        "",
        "Runtime component inventory (normal and build dependency closure; dev-only edges excluded)",
        "",
        *inventories,
        "",
        "Deduplicated license and notice texts",
        "",
    ]
    for digest in sorted(texts):
        lines.extend(
            [
                "=" * 78,
                f"SHA-256: {digest}",
                "Applies to: " + "; ".join(sorted(text_users[digest])),
                "=" * 78,
                texts[digest].rstrip(),
                "",
            ]
        )
    return "\n".join(lines).rstrip() + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-rust-version", required=True)
    parser.add_argument("--cargo", default=os.environ.get("CARGO", "cargo"))
    parser.add_argument("--rustc", default=os.environ.get("RUSTC", "rustc"))
    args = parser.parse_args(argv)
    try:
        notice = build_notice(
            args.source_dir, args.cargo, args.rustc, args.expected_rust_version
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.output.with_name(f".{args.output.name}.tmp")
        temporary.write_text(notice, encoding="utf-8")
        os.chmod(temporary, 0o644)
        os.replace(temporary, args.output)
    except (NoticeError, OSError) as error:
        print(f"license notice error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
