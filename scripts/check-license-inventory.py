#!/usr/bin/env python3
"""Fail closed when migrated or generated runtime sources lack provenance."""

from __future__ import annotations

import fnmatch
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
INVENTORY_PATH = ROOT / "LICENSES/source-inventory.json"


def fail(message: str) -> None:
    raise SystemExit(f"license inventory error: {message}")


def matched(path: str, entry: dict[str, object]) -> bool:
    covered = entry.get("coveredPaths")
    excluded = entry.get("excludedPaths", [])
    if not isinstance(covered, list) or not isinstance(excluded, list):
        fail(f"{entry.get('id', '<unknown>')} has invalid path lists")
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in covered) and not any(
        fnmatch.fnmatchcase(path, pattern) for pattern in excluded
    )


def source_files() -> list[str]:
    roots = [ROOT / "Sources", ROOT / "Tests", ROOT / "guest", ROOT / "scripts"]
    paths = ["Package.swift", "README.md"]
    for base in roots:
        if not base.exists():
            continue
        for item in base.rglob("*"):
            if not item.is_file() or any(part in {".build", "dist", "__pycache__"} for part in item.parts):
                continue
            paths.append(item.relative_to(ROOT).as_posix())
    workflow = ROOT / ".github/workflows"
    if workflow.exists():
        paths.extend(
            item.relative_to(ROOT).as_posix()
            for item in workflow.rglob("*") if item.is_file()
        )
    return sorted(set(paths))


def main() -> None:
    try:
        inventory = json.loads(INVENTORY_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(str(error))
    if inventory.get("schemaVersion") != 1:
        fail("schemaVersion must be 1")
    entries = inventory.get("entries")
    if not isinstance(entries, list):
        fail("entries must be an array")
    by_id = {entry.get("id"): entry for entry in entries if isinstance(entry, dict)}
    required = {"nativepipe-original", "wayland-generated-protocols", "xwayland-satellite"}
    if set(by_id) != required:
        fail(f"expected exactly {sorted(required)}, found {sorted(str(key) for key in by_id)}")

    original = by_id["nativepipe-original"]
    if original.get("licenseExpression") != "LicenseRef-NativePipe-Original":
        fail("original sources must not be assigned an inferred third-party license")
    if "does not infer" not in str(original.get("licenseNotice", "")):
        fail("original-source rights notice is missing")

    protocols = by_id["wayland-generated-protocols"]
    if protocols.get("licenseExpression") != "MIT":
        fail("Wayland protocol sources must retain their embedded MIT notices")
    xwayland = by_id["xwayland-satellite"]
    if xwayland.get("licenseExpression") != "MPL-2.0":
        fail("xwayland-satellite must remain MPL-2.0")
    expected_xwayland_artifacts = [
        "guest/xwayland-satellite/dist/xwayland-satellite-*-gnu",
        "guest/xwayland-satellite/dist/xwayland-satellite-*-musl",
        "guest/xwayland-satellite/dist/xwayland-satellite-*-gnu.third-party-licenses.txt",
        "guest/xwayland-satellite/dist/xwayland-satellite-*-musl.third-party-licenses.txt",
    ]
    if xwayland.get("releaseArtifactPaths") != expected_xwayland_artifacts:
        fail("xwayland-satellite release binaries must be explicitly attributed")
    locked = xwayland.get("lockedCargoAndRustLicenses")
    if not isinstance(locked, dict):
        fail("xwayland-satellite locked Cargo/Rust license policy is missing")
    if locked.get("generator") != "scripts/generate-xwayland-license-notice.py":
        fail("xwayland-satellite license notice generator is not pinned")
    if locked.get("rustVersion") != "1.89.0":
        fail("xwayland-satellite Rust standard-library license version is not pinned")
    if locked.get("rustStandardLibraryLicenseExpression") != "MIT OR Apache-2.0":
        fail("Rust standard-library license expression is unresolved")
    policy = str(locked.get("policy", ""))
    if "non-NOASSERTION" not in policy or "distribution COPYRIGHT" not in policy:
        fail("xwayland-satellite Cargo/Rust license collection policy is incomplete")
    expected_embedded_assets = [
        {
            "path": "OpenSans-Regular.ttf",
            "sha256": "33e93bec67d91c396876db50694213802a39e43a911bb5c22322f0dbdf4d5e43",
            "licenseExpression": "OFL-1.1",
            "notice": (
                "Open Sans 3.003; copyright 2020 The Open Sans Project Authors. "
                "The font's embedded name table declares SIL Open Font License 1.1 "
                "and the exact OFL-1.1 text is bundled in each generated third-party notice."
            ),
        },
        {
            "path": "wl_drm/src/drm.xml",
            "licenseExpression": "MIT",
            "notice": (
                "The generator extracts and bundles the copyright and permission "
                "notice embedded in the pinned protocol XML."
            ),
        },
    ]
    if locked.get("embeddedAssets") != expected_embedded_assets:
        fail("xwayland-satellite embedded font/protocol license policy is incomplete")
    generator = ROOT / str(locked["generator"])
    if not generator.is_file():
        fail("xwayland-satellite license notice generator is missing")
    commit_file = ROOT / str(xwayland.get("sourceCommitFile", ""))
    try:
        upstream_commit = commit_file.read_text(encoding="utf-8").strip()
    except OSError as error:
        fail(f"cannot read xwayland-satellite upstream commit: {error}")
    if len(upstream_commit) != 40 or any(c not in "0123456789abcdef" for c in upstream_commit):
        fail("xwayland-satellite upstream commit must be a lowercase full Git hash")

    for required_policy_path in (
        ".github/workflows/release-nativepipe-runtime.yml",
        ".github/workflows/sign-nativepipe-runtime.yml",
        "scripts/generate-xwayland-license-notice.py",
    ):
        matches = [entry["id"] for entry in entries if matched(required_policy_path, entry)]
        if matches != ["nativepipe-original"]:
            fail(f"release policy provenance is missing or ambiguous: {required_policy_path}")

    license_files = inventory.get("releaseLicenseFiles")
    if not isinstance(license_files, list) or not license_files:
        fail("releaseLicenseFiles must be a non-empty array")
    for relative in license_files:
        if not isinstance(relative, str) or not (ROOT / relative).is_file():
            fail(f"missing release license file: {relative!r}")
    mpl = (ROOT / str(xwayland["licenseFile"])).read_text(encoding="utf-8")
    if "Mozilla Public License Version 2.0" not in mpl or "Exhibit A" not in mpl:
        fail("xwayland-satellite MPL-2.0 text is incomplete")
    ofl = (ROOT / "LICENSES/OFL-1.1.txt").read_text(encoding="utf-8")
    if "SIL OPEN FONT LICENSE Version 1.1" not in ofl or "PERMISSION & CONDITIONS" not in ofl:
        fail("Open Sans OFL-1.1 text is incomplete")

    uncovered: list[str] = []
    ambiguous: list[str] = []
    for path in source_files():
        matches = [entry["id"] for entry in entries if matched(path, entry)]
        if not matches:
            uncovered.append(path)
        elif len(matches) != 1:
            ambiguous.append(f"{path}: {matches}")
    if uncovered:
        fail("uncovered sources:\n  " + "\n  ".join(uncovered))
    if ambiguous:
        fail("sources with ambiguous provenance:\n  " + "\n  ".join(ambiguous))

    protocol_paths = [
        path for path in source_files()
        if fnmatch.fnmatchcase(path, "guest/compositor/*-protocol.c")
        or fnmatch.fnmatchcase(path, "guest/compositor/*-protocol.h")
        or fnmatch.fnmatchcase(path, "guest/compositor/*.xml")
    ]
    if not protocol_paths:
        fail("no generated/upstream Wayland protocol sources were found")
    for path in protocol_paths:
        text = (ROOT / path).read_text(encoding="utf-8")
        if not any(phrase in text for phrase in (
            "Permission is hereby granted, free of charge",
            "Permission to use, copy, modify, distribute, and sell",
        )):
            fail(f"protocol license notice missing from {path}")
        if path.endswith(("-protocol.c", "-protocol.h")) and "Generated by wayland-scanner" not in text:
            fail(f"generated-source marker missing from {path}")

    print(f"license inventory OK: {len(source_files())} source files, {len(protocol_paths)} protocol files")


if __name__ == "__main__":
    main()
