#!/usr/bin/env python3
"""Offline verifier for a NativePipe guest-display runtime release target."""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import tempfile
from typing import Any

from package_nativepipe_runtime import (
    COMMIT_RE,
    COMPONENT,
    GUEST_RUNTIME_ABI,
    MANDATORY_RELEASE_LICENSES,
    MAXIMUM_ARCHIVE_BYTES,
    MAXIMUM_FILES,
    MAXIMUM_MANIFEST_BYTES,
    MAXIMUM_RELEASE_SEQUENCE,
    PackagingError,
    PACKAGE_LICENSE,
    REPOSITORY_RE,
    REQUIRED_RUNTIME_PREFIXES,
    SCHEMA_VERSION,
    VERSION_RE,
    canonical_json_bytes,
    sha256_file,
    validate_asset_name,
    validate_mode,
    validate_relative_path,
    validate_runtime_install_path,
    required_runtime_files,
    spdx_copyright_for_path,
    spdx_license_for_path,
)


MANIFEST_FIELDS = {
    "schemaVersion",
    "component",
    "version",
    "releaseTag",
    "releaseSequence",
    "sourceRepository",
    "sourceCommit",
    "architecture",
    "guestRuntimeABI",
    "archive",
    "files",
}


def require_exact_int(value: Any, label: str, *, positive: bool = False) -> int:
    if type(value) is not int:
        raise PackagingError(f"{label} must be a JSON integer")
    if positive and value <= 0:
        raise PackagingError(f"{label} must be positive")
    if not positive and value < 0:
        raise PackagingError(f"{label} must be non-negative")
    return value


def require_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise PackagingError(f"{label} must be a non-empty string")
    return value


def read_json(path: Path, label: str) -> Any:
    try:
        with path.open("r", encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise PackagingError(f"cannot read {label} {path}: {error}") from error


def verify_reference(asset_dir: Path, reference: Any, label: str) -> Path:
    if not isinstance(reference, dict):
        raise PackagingError(f"{label} must be an object")
    name = validate_asset_name(require_string(reference.get("name"), f"{label}.name"), f"{label}.name")
    expected_size = require_exact_int(reference.get("size"), f"{label}.size")
    if label == "archive" and (
        expected_size <= 0 or expected_size > MAXIMUM_ARCHIVE_BYTES
    ):
        raise PackagingError("archive size is outside the host limit")
    expected_sha256 = require_string(reference.get("sha256"), f"{label}.sha256")
    if len(expected_sha256) != 64 or any(c not in "0123456789abcdef" for c in expected_sha256):
        raise PackagingError(f"{label}.sha256 must be lowercase hexadecimal SHA-256")
    path = asset_dir / name
    try:
        metadata = path.lstat()
    except FileNotFoundError as error:
        raise PackagingError(f"referenced {label} is missing: {name}") from error
    if not path.is_file() or path.is_symlink():
        raise PackagingError(f"referenced {label} is not a regular file: {name}")
    if metadata.st_size != expected_size:
        raise PackagingError(
            f"{label} size mismatch: expected {expected_size}, got {metadata.st_size}"
        )
    actual_sha256 = sha256_file(path)
    if actual_sha256 != expected_sha256:
        raise PackagingError(
            f"{label} SHA-256 mismatch: expected {expected_sha256}, got {actual_sha256}"
        )
    return path


def regular_sibling(asset_dir: Path, name: str, label: str) -> Path:
    validate_asset_name(name, label)
    path = asset_dir / name
    try:
        metadata = path.lstat()
    except FileNotFoundError as error:
        raise PackagingError(f"required sibling {label} is missing: {name}") from error
    if path.is_symlink() or not path.is_file() or metadata.st_size == 0:
        raise PackagingError(f"required sibling {label} must be a non-empty regular file")
    return path


def read_ed25519_signature(path: Path) -> bytes:
    signature = path.read_bytes()
    if len(signature) == 64:
        return signature
    try:
        decoded = base64.b64decode(signature.strip(), validate=True)
    except (binascii.Error, ValueError, TypeError) as error:
        raise PackagingError("manifest signature is neither raw nor base64 Ed25519") from error
    if len(decoded) != 64:
        raise PackagingError("manifest signature must contain exactly 64 bytes")
    return decoded


def verify_ed25519_signature(
    manifest_path: Path,
    signature_path: Path,
    public_key_path: Path,
) -> None:
    try:
        metadata = public_key_path.lstat()
    except FileNotFoundError as error:
        raise PackagingError(f"Ed25519 public key is missing: {public_key_path}") from error
    if public_key_path.is_symlink() or not public_key_path.is_file():
        raise PackagingError("Ed25519 public key must be a regular file")
    if metadata.st_size <= 0 or metadata.st_size > 16 * 1024:
        raise PackagingError("Ed25519 public key size is outside the verifier limit")

    openssl = os.environ.get("OPENSSL", "openssl")
    try:
        public_der = subprocess.run(
            [
                openssl,
                "pkey",
                "-pubin",
                "-in",
                str(public_key_path),
                "-outform",
                "DER",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise PackagingError(f"cannot execute OpenSSL: {error}") from error
    ed25519_spki_prefix = bytes.fromhex("302a300506032b6570032100")
    if (
        public_der.returncode != 0
        or len(public_der.stdout) != len(ed25519_spki_prefix) + 32
        or not public_der.stdout.startswith(ed25519_spki_prefix)
    ):
        raise PackagingError("public key is not an Ed25519 public PEM")

    signature = read_ed25519_signature(signature_path)
    with tempfile.TemporaryDirectory(prefix="nativepipe-runtime-signature-") as temporary:
        normalized_signature = Path(temporary) / "manifest.sig"
        normalized_signature.write_bytes(signature)
        try:
            verified = subprocess.run(
                [
                    openssl,
                    "pkeyutl",
                    "-verify",
                    "-rawin",
                    "-pubin",
                    "-inkey",
                    str(public_key_path),
                    "-in",
                    str(manifest_path),
                    "-sigfile",
                    str(normalized_signature),
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as error:
            raise PackagingError(f"cannot execute OpenSSL: {error}") from error
    if verified.returncode != 0:
        raise PackagingError("manifest Ed25519 signature verification failed")


def verify_raw_ustar(path: Path) -> None:
    data = path.read_bytes()
    if len(data) == 0 or len(data) % 512 != 0:
        raise PackagingError("archive is not a non-empty 512-byte-aligned tar stream")
    offset = 0
    zero_blocks = 0
    while offset < len(data):
        header = data[offset : offset + 512]
        if header == bytes(512):
            zero_blocks += 1
            offset += 512
            if any(data[offset:]):
                continue
            break
        if zero_blocks:
            raise PackagingError("archive has data after a zero end-of-archive block")
        if header[257:263] != b"ustar\x00" or header[263:265] != b"00":
            raise PackagingError("archive contains a non-USTAR header")
        type_flag = header[156:157]
        if type_flag not in (b"\x00", b"0", b"5"):
            raise PackagingError(
                f"archive contains a non-regular/non-directory header type {type_flag!r}"
            )
        raw_size = header[124:136].rstrip(b"\x00 ").lstrip(b" ") or b"0"
        try:
            size = int(raw_size, 8)
        except ValueError as error:
            raise PackagingError("archive contains an invalid USTAR size field") from error
        data_offset = offset + 512
        next_offset = data_offset + ((size + 511) // 512) * 512
        if any(data[data_offset + size : next_offset]):
            raise PackagingError("archive contains non-zero file padding")
        offset = next_offset
    if zero_blocks < 1 or not all(byte == 0 for byte in data[offset:]):
        raise PackagingError("archive is missing its zero-filled end marker")
    if len(data) - (offset - 512) < 1024:
        raise PackagingError("archive has fewer than two zero end-of-archive blocks")


def verify_archive(
    archive_path: Path,
    manifest_files: list[dict[str, Any]],
    source_date_epoch: int,
) -> None:
    verify_raw_ustar(archive_path)
    expected = {entry["path"]: entry for entry in manifest_files}
    observed_files: dict[str, dict[str, Any]] = {}
    observed_directories: set[str] = set()
    member_names: list[str] = []
    try:
        with tarfile.open(archive_path, mode="r:") as archive:
            for member in archive.getmembers():
                name = validate_relative_path(member.name, "archive member name")
                if name in member_names:
                    raise PackagingError(f"duplicate archive member: {name}")
                member_names.append(name)
                if member.uid != 0 or member.gid != 0 or member.uname or member.gname:
                    raise PackagingError(f"archive member has non-canonical ownership: {name}")
                if member.mtime != source_date_epoch:
                    raise PackagingError(f"archive member has non-canonical mtime: {name}")
                if member.isdir():
                    if member.mode != 0o755:
                        raise PackagingError(f"archive directory has non-canonical mode: {name}")
                    if member.size != 0:
                        raise PackagingError(f"archive directory has non-zero size: {name}")
                    observed_directories.add(name)
                    continue
                if not member.isreg():
                    raise PackagingError(f"archive contains a non-regular entry: {name}")
                extracted = archive.extractfile(member)
                if extracted is None:
                    raise PackagingError(f"cannot read archive file: {name}")
                digest = hashlib.sha256()
                size = 0
                for chunk in iter(lambda: extracted.read(1024 * 1024), b""):
                    size += len(chunk)
                    digest.update(chunk)
                observed_files[name] = {
                    "path": name,
                    "size": size,
                    "sha256": digest.hexdigest(),
                    "mode": member.mode,
                }
    except (OSError, tarfile.TarError) as error:
        raise PackagingError(f"cannot inspect USTAR archive: {error}") from error
    if member_names != sorted(member_names):
        raise PackagingError("archive members are not in deterministic lexical order")
    if set(observed_files) != set(expected):
        missing = sorted(set(expected) - set(observed_files))
        extra = sorted(set(observed_files) - set(expected))
        raise PackagingError(f"archive file set mismatch; missing={missing}, extra={extra}")
    expected_directories: set[str] = set()
    for name in expected:
        parent = PurePosixPath(name).parent
        while str(parent) != ".":
            expected_directories.add(parent.as_posix())
            parent = parent.parent
    if observed_directories != expected_directories:
        missing = sorted(expected_directories - observed_directories)
        extra = sorted(observed_directories - expected_directories)
        raise PackagingError(
            f"archive directory set mismatch; missing={missing}, extra={extra}"
        )
    for name, expected_entry in expected.items():
        if observed_files[name] != expected_entry:
            raise PackagingError(
                f"archive file metadata mismatch for {name}: "
                f"expected {expected_entry}, got {observed_files[name]}"
            )


def verify_manifest(
    manifest_path: Path,
    asset_dir: Path | None = None,
    *,
    signature_public_key: Path | None = None,
) -> None:
    try:
        manifest_size = manifest_path.lstat().st_size
    except FileNotFoundError as error:
        raise PackagingError(f"runtime manifest is missing: {manifest_path}") from error
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise PackagingError("runtime manifest must be a regular file")
    if manifest_size <= 0 or manifest_size > MAXIMUM_MANIFEST_BYTES:
        raise PackagingError("runtime manifest size is outside the host limit")
    manifest = read_json(manifest_path, "runtime manifest")
    if not isinstance(manifest, dict):
        raise PackagingError("runtime manifest must be an object")
    if set(manifest) != MANIFEST_FIELDS:
        missing = sorted(MANIFEST_FIELDS - set(manifest))
        extra = sorted(set(manifest) - MANIFEST_FIELDS)
        raise PackagingError(
            f"runtime manifest field set mismatch; missing={missing}, extra={extra}"
        )
    if manifest.get("schemaVersion") != SCHEMA_VERSION or type(manifest.get("schemaVersion")) is not int:
        raise PackagingError(f"schemaVersion must be the integer {SCHEMA_VERSION}")
    if manifest.get("component") != COMPONENT:
        raise PackagingError(f"component must be {COMPONENT!r}")
    if manifest.get("guestRuntimeABI") != GUEST_RUNTIME_ABI or type(manifest.get("guestRuntimeABI")) is not int:
        raise PackagingError(f"guestRuntimeABI must be the integer {GUEST_RUNTIME_ABI}")
    version = require_string(manifest.get("version"), "version")
    if not VERSION_RE.fullmatch(version):
        raise PackagingError("version is not a supported semantic version")
    release_sequence = require_exact_int(
        manifest.get("releaseSequence"), "releaseSequence", positive=True
    )
    if release_sequence > MAXIMUM_RELEASE_SEQUENCE:
        raise PackagingError("releaseSequence exceeds UInt64")
    release_tag = require_string(manifest.get("releaseTag"), "releaseTag")
    if release_tag != f"nativepipe-runtime-v{version}-{release_sequence}":
        raise PackagingError("releaseTag does not match version and releaseSequence")
    source_repository = require_string(manifest.get("sourceRepository"), "sourceRepository")
    source_commit = require_string(manifest.get("sourceCommit"), "sourceCommit")
    if not REPOSITORY_RE.fullmatch(source_repository):
        raise PackagingError("sourceRepository must use the exact owner/repository form")
    if not COMMIT_RE.fullmatch(source_commit):
        raise PackagingError("sourceCommit must be lowercase 40-character hexadecimal")
    architecture = manifest.get("architecture")
    if architecture not in ("aarch64", "x86_64"):
        raise PackagingError("architecture is unsupported")
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise PackagingError("files must be a non-empty array")
    if len(files) > MAXIMUM_FILES:
        raise PackagingError("files exceeds the host file-count limit")
    normalized_files: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, entry in enumerate(files):
        label = f"files[{index}]"
        if not isinstance(entry, dict):
            raise PackagingError(f"{label} must be an object")
        path = validate_runtime_install_path(
            require_string(entry.get("path"), f"{label}.path"), f"{label}.path"
        )
        if path in seen:
            raise PackagingError(f"duplicate manifest path: {path}")
        seen.add(path)
        size = require_exact_int(entry.get("size"), f"{label}.size")
        if size > MAXIMUM_ARCHIVE_BYTES:
            raise PackagingError(f"{label}.size exceeds the host limit")
        mode = validate_mode(entry.get("mode"), f"{label}.mode")
        sha256 = require_string(entry.get("sha256"), f"{label}.sha256")
        if len(sha256) != 64 or any(c not in "0123456789abcdef" for c in sha256):
            raise PackagingError(f"{label}.sha256 must be lowercase hexadecimal SHA-256")
        normalized_files.append(
            {"path": path, "size": size, "sha256": sha256, "mode": mode}
        )
    if sum(entry["size"] for entry in normalized_files) > MAXIMUM_ARCHIVE_BYTES:
        raise PackagingError("manifest expanded size exceeds the host limit")
    if [entry["path"] for entry in normalized_files] != sorted(seen):
        raise PackagingError("manifest files are not in deterministic lexical order")
    for prefix in REQUIRED_RUNTIME_PREFIXES:
        if not any(entry["path"].startswith(prefix) for entry in normalized_files):
            raise PackagingError(f"manifest omits required runtime prefix: {prefix}")
    files_by_path = {entry["path"]: entry for entry in normalized_files}
    for required_path, required_mode in required_runtime_files(architecture).items():
        if required_path not in files_by_path:
            raise PackagingError(f"manifest omits required dual-libc runtime file: {required_path}")
        if files_by_path[required_path]["mode"] != required_mode:
            raise PackagingError(
                f"required runtime file has wrong mode: {required_path}"
            )
    for license_path in MANDATORY_RELEASE_LICENSES:
        if license_path not in files_by_path:
            raise PackagingError(f"manifest omits required release license: {license_path}")
        if files_by_path[license_path]["mode"] != 0o644:
            raise PackagingError(f"release license has unsafe mode: {license_path}")

    asset_dir = (asset_dir or manifest_path.parent).resolve()
    base_name = f"{COMPONENT}-{architecture}"
    if manifest_path.name != f"{base_name}.runtime-manifest.json":
        raise PackagingError("runtime manifest file name does not match its target identity")
    archive_reference = manifest.get("archive")
    if not isinstance(archive_reference, dict) or archive_reference.get("name") != f"{base_name}.tar":
        raise PackagingError(f"archive asset name must be exactly {base_name}.tar")
    archive_path = verify_reference(asset_dir, manifest.get("archive"), "archive")
    tests_path = regular_sibling(asset_dir, f"{base_name}.tests.json", "tests")
    sbom_path = regular_sibling(asset_dir, f"{base_name}.spdx.json", "SBOM")
    source_path = regular_sibling(asset_dir, f"{base_name}.source.json", "source")
    provenance_path = regular_sibling(
        asset_dir, f"{base_name}.provenance.json", "provenance"
    )
    if signature_public_key is not None:
        signature_path = regular_sibling(
            asset_dir, f"{base_name}.runtime-manifest.sig", "manifest signature"
        )
        verify_ed25519_signature(
            manifest_path,
            signature_path,
            signature_public_key.absolute(),
        )

    source = read_json(source_path, "source metadata")
    if not isinstance(source, dict):
        raise PackagingError("source metadata must be an object")
    if source.get("schemaVersion") != SCHEMA_VERSION or source.get("component") != COMPONENT:
        raise PackagingError("source metadata schema identity is invalid")
    if (
        source.get("sourceRepository") != source_repository
        or source.get("sourceCommit") != source_commit
    ):
        raise PackagingError("source metadata does not match manifest source identity")
    if (
        source.get("version") != version
        or source.get("releaseTag") != release_tag
        or source.get("releaseSequence") != release_sequence
        or source.get("architecture") != architecture
    ):
        raise PackagingError("source metadata release identity does not match manifest")
    source_date_epoch = require_exact_int(
        source.get("sourceDateEpoch"), "source.sourceDateEpoch"
    )
    plan_sha256 = source.get("payloadPlanSha256")
    if not isinstance(plan_sha256, str) or len(plan_sha256) != 64 or any(
        character not in "0123456789abcdef" for character in plan_sha256
    ):
        raise PackagingError("source payloadPlanSha256 is invalid")
    license_reference = source.get("licenseInventory")
    inventory_entry = files_by_path[MANDATORY_RELEASE_LICENSES[0]]
    if license_reference != {
        "path": inventory_entry["path"],
        "size": inventory_entry["size"],
        "sha256": inventory_entry["sha256"],
    }:
        raise PackagingError("source license inventory reference does not match the archive")
    verify_archive(archive_path, normalized_files, source_date_epoch)

    tests = read_json(tests_path, "test metadata")
    if not isinstance(tests, dict) or tests.get("architecture") != architecture:
        raise PackagingError("test metadata architecture does not match manifest")
    if (
        tests.get("schemaVersion") != SCHEMA_VERSION
        or tests.get("component") != COMPONENT
        or tests.get("version") != version
    ):
        raise PackagingError("test metadata release identity does not match manifest")
    if tests.get("sourceCommit") != source_commit:
        raise PackagingError("test metadata source commit does not match manifest")
    results = tests.get("results")
    if not isinstance(results, dict) or results.get("status") != "passed":
        raise PackagingError("test metadata does not record an explicit passed status")

    sbom = read_json(sbom_path, "SPDX metadata")
    if not isinstance(sbom, dict) or sbom.get("spdxVersion") != "SPDX-2.3":
        raise PackagingError("SBOM is not an SPDX 2.3 JSON document")
    if sbom.get("name") != f"{COMPONENT}-{version}-{architecture}":
        raise PackagingError("SBOM architecture identity does not match manifest")
    sbom_files = sbom.get("files")
    if not isinstance(sbom_files, list):
        raise PackagingError("SPDX files must be an array")
    packages = sbom.get("packages")
    if not isinstance(packages, list) or len(packages) != 1:
        raise PackagingError("SPDX must describe exactly one runtime package")
    package = packages[0]
    if not isinstance(package, dict) or (
        package.get("licenseConcluded") != PACKAGE_LICENSE
        or package.get("licenseDeclared") != PACKAGE_LICENSE
    ):
        raise PackagingError("SPDX package license is unresolved or unexpected")
    extracted = sbom.get("hasExtractedLicensingInfos")
    if not isinstance(extracted, list):
        raise PackagingError("SPDX custom license information is missing")
    extracted_ids = {
        item.get("licenseId")
        for item in extracted
        if isinstance(item, dict)
        and isinstance(item.get("extractedText"), str)
        and item.get("extractedText")
        and "NOASSERTION" not in item.get("extractedText", "")
    }
    required_license_refs = {
        component
        for component in PACKAGE_LICENSE.split()
        if component.startswith("LicenseRef-")
    }
    if extracted_ids != required_license_refs:
        raise PackagingError("SPDX custom license references are incomplete")

    sbom_by_path: dict[str, dict[str, Any]] = {}
    for index, sbom_entry in enumerate(sbom_files):
        if not isinstance(sbom_entry, dict):
            raise PackagingError(f"SPDX files[{index}] must be an object")
        file_name = sbom_entry.get("fileName")
        if not isinstance(file_name, str) or not file_name.startswith("./"):
            raise PackagingError(f"SPDX files[{index}].fileName is invalid")
        file_path = file_name[2:]
        if file_path in sbom_by_path:
            raise PackagingError(f"SPDX contains duplicate file entry: {file_path}")
        sbom_by_path[file_path] = sbom_entry
    if set(sbom_by_path) != set(seen):
        raise PackagingError("SPDX file set does not match manifest")
    for entry in normalized_files:
        sbom_entry = sbom_by_path[entry["path"]]
        checksums = sbom_entry.get("checksums", [])
        if {
            "algorithm": "SHA256",
            "checksumValue": entry["sha256"],
        } not in checksums:
            raise PackagingError(f"SPDX SHA-256 mismatch for {entry['path']}")
        expected_license = spdx_license_for_path(entry["path"])
        if (
            sbom_entry.get("licenseConcluded") != expected_license
            or sbom_entry.get("licenseInfoInFiles") != [expected_license]
        ):
            raise PackagingError(
                f"SPDX file license is unresolved or unexpected for {entry['path']}"
            )
        expected_copyright = spdx_copyright_for_path(entry["path"])
        if sbom_entry.get("copyrightText") != expected_copyright:
            raise PackagingError(
                f"SPDX file copyright is unresolved or unexpected for {entry['path']}"
            )

    provenance = read_json(provenance_path, "provenance metadata")
    if not isinstance(provenance, dict) or provenance.get("_type") != "https://in-toto.io/Statement/v1":
        raise PackagingError("provenance is not an in-toto Statement v1")
    expected_subject = {
        "name": archive_path.name,
        "digest": {"sha256": manifest["archive"]["sha256"]},
    }
    if expected_subject not in provenance.get("subject", []):
        raise PackagingError("provenance does not identify the authoritative archive")
    predicate = provenance.get("predicate")
    if not isinstance(predicate, dict):
        raise PackagingError("provenance predicate is missing")
    build_definition = predicate.get("buildDefinition")
    if not isinstance(build_definition, dict):
        raise PackagingError("provenance build definition is missing")
    parameters = build_definition.get("externalParameters")
    if parameters != {
        "releaseTag": release_tag,
        "releaseSequence": release_sequence,
        "architecture": architecture,
    }:
        raise PackagingError("provenance release parameters do not match manifest")
    expected_dependency = {
        "uri": source_repository,
        "digest": {"gitCommit": source_commit},
    }
    if expected_dependency not in build_definition.get("resolvedDependencies", []):
        raise PackagingError("provenance source identity does not match manifest")

    # Canonical JSON is part of reproducibility and prevents signature inputs
    # from changing because of formatter differences.
    for path, value, label in (
        (manifest_path, manifest, "manifest"),
        (tests_path, tests, "tests"),
        (sbom_path, sbom, "SBOM"),
        (source_path, source, "source"),
        (provenance_path, provenance, "provenance"),
    ):
        if path.read_bytes() != canonical_json_bytes(value):
            raise PackagingError(f"{label} JSON is not canonical")


def parser() -> argparse.ArgumentParser:
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument("manifest", type=Path)
    argument_parser.add_argument(
        "--asset-dir",
        type=Path,
        help="directory containing referenced assets (defaults to manifest directory)",
    )
    argument_parser.add_argument(
        "--require-signature",
        type=Path,
        metavar="PUBLIC_KEY_PEM",
        help=(
            "require the fixed-name manifest signature sibling and verify it "
            "with this Ed25519 public PEM"
        ),
    )
    return argument_parser


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        verify_manifest(
            args.manifest.absolute(),
            args.asset_dir,
            signature_public_key=args.require_signature,
        )
    except PackagingError as error:
        print(f"error: {error}", file=os.sys.stderr)
        return 2
    print(f"verified={args.manifest.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
