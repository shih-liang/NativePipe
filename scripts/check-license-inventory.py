#!/usr/bin/env python3
"""Fail closed when migrated or generated runtime sources lack provenance."""

from __future__ import annotations

import fnmatch
import hashlib
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
INVENTORY_PATH = ROOT / "LICENSES/source-inventory.json"
# Unmodified text from https://raw.githubusercontent.com/spdx/license-list-data/main/text/AGPL-3.0-only.txt
AGPL3_SHA256 = "d8a6cc31abc16b6748c7a21f21611f5a1ec33f67d22ca23d7da1c19b95496bee"


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
    roots = [ROOT / "Sources", ROOT / "Tests", ROOT / "guest", ROOT / "scripts", ROOT / "common"]
    paths = ["Package.swift", "README.md"]
    for base in roots:
        if not base.exists():
            continue
        for item in base.rglob("*"):
            if not item.is_file() or any(part in {".build", "build", "dist", "__pycache__"} for part in item.parts):
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
    required = {"nativepipe-original", "wayland-generated-protocols", "nvidia-nvenc-header"}
    if set(by_id) != required:
        fail(f"expected exactly {sorted(required)}, found {sorted(str(key) for key in by_id)}")

    original = by_id["nativepipe-original"]
    if original.get("licenseExpression") != "AGPL-3.0-only":
        fail("original sources must use the project's AGPL-3.0-only license")
    if original.get("licenseLocation") != "LICENSE":
        fail("original-source license must point to LICENSE")
    if "LICENSES/NOTICE" not in str(original.get("licenseNotice", "")):
        fail("original-source licensing notice is missing")

    nvenc = by_id["nvidia-nvenc-header"]
    header = ROOT / "guest/encoder/vendor/nvEncodeAPI.h"
    if nvenc.get("licenseExpression") != "MIT" or nvenc.get("sha256") != hashlib.sha256(header.read_bytes()).hexdigest():
        fail("NVENC header license or pinned source hash does not match")

    protocols = by_id["wayland-generated-protocols"]
    if protocols.get("licenseExpression") != "MIT":
        fail("Wayland protocol sources must retain their embedded MIT notices")
    for required_policy_path in (".github/workflows/build-linux.yml",):
        matches = [entry["id"] for entry in entries if matched(required_policy_path, entry)]
        if matches != ["nativepipe-original"]:
            fail(f"release policy provenance is missing or ambiguous: {required_policy_path}")

    license_files = inventory.get("releaseLicenseFiles")
    if not isinstance(license_files, list) or not license_files:
        fail("releaseLicenseFiles must be a non-empty array")
    for relative in license_files:
        if not isinstance(relative, str) or not (ROOT / relative).is_file():
            fail(f"missing release license file: {relative!r}")
        if not (ROOT / relative).stat().st_size:
            fail(f"empty release license file: {relative}")
    if not {"LICENSE", "LICENSES/NOTICE", "LICENSES/source-inventory.json"} <= set(license_files):
        fail("release license files must include the license, licensing notice and source inventory")
    if hashlib.sha256((ROOT / "LICENSE").read_bytes()).hexdigest() != AGPL3_SHA256:
        fail("LICENSE must contain the complete, unmodified AGPLv3 text")
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
