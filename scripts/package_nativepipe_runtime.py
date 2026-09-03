#!/usr/bin/env python3
"""Build a deterministic NativePipe guest-display runtime release.

The uncompressed USTAR archive is the authoritative install artifact.  The
runtime manifest is intentionally external to the archive: including the
manifest in the archive would make its archive digest self-referential.

Only Python's standard library is used so this can run in a clean release job.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tarfile
import tempfile
from typing import Any, Iterable


SCHEMA_VERSION = 1
GUEST_RUNTIME_ABI = 1
COMPONENT = "nativepipe-runtime"
MAXIMUM_MANIFEST_BYTES = 1 << 20
MAXIMUM_ARCHIVE_BYTES = 512 << 20
MAXIMUM_FILES = 4_096
MAXIMUM_PATH_BYTES = 512
MAXIMUM_RELEASE_SEQUENCE = (1 << 64) - 1
REQUIRED_RUNTIME_PREFIXES = (
    "guest/session/",
    "guest/xwayland-satellite/",
    "Packages/NativePipe/guest/compositor/",
)
LICENSE_INVENTORY_SOURCE = "LICENSES/source-inventory.json"
RELEASE_LICENSE_PREFIX = "LICENSES/nativepipe-runtime/"
MANDATORY_RELEASE_LICENSE_SOURCES = (
    "LICENSES/source-inventory.json",
    "LICENSES/OFL-1.1.txt",
    "LICENSES/xwayland-satellite-MPL-2.0.txt",
)
MANDATORY_RELEASE_LICENSES = tuple(
    RELEASE_LICENSE_PREFIX + PurePosixPath(path).name
    for path in MANDATORY_RELEASE_LICENSE_SOURCES
)
ORIGINAL_LICENSE = "LicenseRef-NativePipe-Original"
XWAYLAND_DEPENDENCY_LICENSE = "LicenseRef-XwaylandSatellite-Cargo-And-Rust"
PACKAGE_LICENSE = (
    f"{ORIGINAL_LICENSE} AND MIT AND MPL-2.0 AND Apache-2.0 AND "
    f"{XWAYLAND_DEPENDENCY_LICENSE}"
)
VERSION_RE = re.compile(
    r"^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
    r"(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$"
)
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")


class PackagingError(ValueError):
    """A release input is invalid or unsafe."""


def canonical_json_bytes(value: Any) -> bytes:
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        + "\n"
    ).encode("utf-8")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha1_file(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_bytes(path: Path, data: bytes, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary_path = Path(temporary)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary_path, mode)
        os.replace(temporary_path, path)
    finally:
        temporary_path.unlink(missing_ok=True)


def write_json(path: Path, value: Any) -> None:
    write_bytes(path, canonical_json_bytes(value))


def validate_relative_path(value: str, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise PackagingError(f"{label} must be a non-empty string")
    if "\\" in value or "\x00" in value or value.startswith("/"):
        raise PackagingError(f"{label} must be a relative POSIX path: {value!r}")
    if len(value.encode("utf-8")) > MAXIMUM_PATH_BYTES:
        raise PackagingError(f"{label} exceeds {MAXIMUM_PATH_BYTES} UTF-8 bytes")
    components = value.split("/")
    if any(component in ("", ".", "..") for component in components):
        raise PackagingError(f"{label} contains an unsafe component: {value!r}")
    normalized = str(PurePosixPath(value))
    if normalized != value:
        raise PackagingError(f"{label} is not normalized: {value!r}")
    return value


def validate_runtime_install_path(value: str, label: str) -> str:
    value = validate_relative_path(value, label)
    forbidden = {
        "GuestRuntimeSet.plist",
        "GuestRuntimeSet.json",
        "GuestRuntime.lock.json",
        "MANIFEST.sha256",
        ".metadata",
    }
    if value in forbidden or value.startswith(".metadata/"):
        raise PackagingError(f"{label} is reserved by the FluxWindow runtime store")
    return value


def validate_asset_name(value: str, label: str) -> str:
    validate_relative_path(value, label)
    if "/" in value:
        raise PackagingError(f"{label} must be a file name, not a path")
    return value


def ensure_regular_source(repo_root: Path, relative: str, label: str) -> Path:
    validate_relative_path(relative, label)
    candidate = repo_root.joinpath(*PurePosixPath(relative).parts)
    try:
        metadata = candidate.lstat()
    except FileNotFoundError as error:
        raise PackagingError(f"{label} does not exist: {relative}") from error
    if stat.S_ISLNK(metadata.st_mode):
        raise PackagingError(f"{label} must not be a symbolic link: {relative}")
    if not stat.S_ISREG(metadata.st_mode):
        raise PackagingError(f"{label} must be a regular file: {relative}")
    resolved_root = repo_root.resolve()
    resolved_candidate = candidate.resolve()
    try:
        resolved_candidate.relative_to(resolved_root)
    except ValueError as error:
        raise PackagingError(f"{label} resolves outside the repository: {relative}") from error
    return candidate


def validate_mode(value: Any, label: str) -> int:
    # bool is an int subclass, but is never a meaningful filesystem mode.
    if type(value) is not int:
        raise PackagingError(f"{label} must be a JSON integer")
    if value not in (0o644, 0o755):
        raise PackagingError(f"{label} must be the JSON integer 420 or 493")
    return value


def required_runtime_files(architecture: str) -> dict[str, int]:
    session = "guest/session/dist"
    xwayland = "guest/xwayland-satellite/dist"
    compositor = "Packages/NativePipe/guest/compositor/dist"
    return {
        f"{session}/nativepipe-session-{architecture}": 0o755,
        f"{session}/nativepipe-align-blob-{architecture}-gnu.so": 0o644,
        f"{session}/nativepipe-align-blob-{architecture}-musl.so": 0o644,
        f"{session}/nativepipe-vulkan-layer-{architecture}-gnu.so": 0o644,
        f"{session}/nativepipe-vulkan-layer-{architecture}-musl.so": 0o644,
        f"{xwayland}/xwayland-satellite-{architecture}-gnu": 0o755,
        f"{xwayland}/xwayland-satellite-{architecture}-musl": 0o755,
        f"{xwayland}/xwayland-satellite-{architecture}-gnu.third-party-licenses.txt": 0o644,
        f"{xwayland}/xwayland-satellite-{architecture}-musl.third-party-licenses.txt": 0o644,
        f"{compositor}/vmpipe-wayland-{architecture}-gnu": 0o755,
        f"{compositor}/vmpipe-wayland-{architecture}-musl": 0o755,
    }


def spdx_license_for_path(path: str) -> str:
    if path.endswith("/xwayland-satellite-MPL-2.0.txt"):
        return "MPL-2.0"
    if path.startswith("guest/xwayland-satellite/dist/xwayland-satellite-"):
        if path.endswith(".third-party-licenses.txt"):
            return XWAYLAND_DEPENDENCY_LICENSE
        return f"MPL-2.0 AND MIT AND Apache-2.0 AND {XWAYLAND_DEPENDENCY_LICENSE}"
    if path.startswith("Packages/NativePipe/guest/compositor/dist/"):
        return f"{ORIGINAL_LICENSE} AND MIT"
    return ORIGINAL_LICENSE


def spdx_copyright_for_path(path: str) -> str:
    if path.startswith("guest/xwayland-satellite/dist/xwayland-satellite-"):
        return "Copyright holders identified in the bundled xwayland-satellite notices"
    if path.endswith("/xwayland-satellite-MPL-2.0.txt"):
        return "Copyright holders identified in the bundled xwayland-satellite notices"
    if path.startswith("Packages/NativePipe/guest/compositor/dist/"):
        return "Copyright holders identified in the source and bundled license inventory"
    return "Copyright NativePipe project authors"


def read_json(path: Path, label: str) -> Any:
    try:
        with path.open("r", encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise PackagingError(f"cannot read {label} {path}: {error}") from error


def iso8601_utc(epoch: int) -> str:
    try:
        instant = dt.datetime.fromtimestamp(epoch, tz=dt.timezone.utc)
    except (OverflowError, OSError, ValueError) as error:
        raise PackagingError(f"SOURCE_DATE_EPOCH is outside the supported range: {epoch}") from error
    return instant.strftime("%Y-%m-%dT%H:%M:%SZ")


def metadata_reference(path: Path) -> dict[str, Any]:
    return {
        "name": path.name,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def load_payload(
    repo_root: Path,
    plan_path: Path,
    license_inventory: str,
    architecture: str,
) -> list[dict[str, Any]]:
    if license_inventory != LICENSE_INVENTORY_SOURCE:
        raise PackagingError(
            "release license inventory path must be "
            f"{LICENSE_INVENTORY_SOURCE!r}"
        )
    plan = read_json(plan_path, "payload plan")
    if not isinstance(plan, dict) or plan.get("schemaVersion") != 1:
        raise PackagingError("payload plan must be an object with schemaVersion 1")
    raw_files = plan.get("files")
    if not isinstance(raw_files, list) or not raw_files:
        raise PackagingError("payload plan files must be a non-empty array")

    inventory_path = ensure_regular_source(
        repo_root, license_inventory, "license inventory"
    )
    inventory = read_json(inventory_path, "license inventory")
    if not isinstance(inventory, dict) or inventory.get("schemaVersion") != 1:
        raise PackagingError("license inventory must be an object with schemaVersion 1")
    release_license_files = inventory.get("releaseLicenseFiles")
    if not isinstance(release_license_files, list) or not release_license_files:
        raise PackagingError("license inventory releaseLicenseFiles must be non-empty")
    if any(not isinstance(path, str) for path in release_license_files):
        raise PackagingError("license inventory releaseLicenseFiles must contain strings")
    if license_inventory not in release_license_files:
        raise PackagingError("the license inventory must list itself in releaseLicenseFiles")
    missing_mandatory = sorted(
        set(MANDATORY_RELEASE_LICENSE_SOURCES) - set(release_license_files)
    )
    if missing_mandatory:
        raise PackagingError(
            "license inventory omits required release file(s): "
            + ", ".join(missing_mandatory)
        )
    raw_files = list(raw_files)
    for release_license in release_license_files:
        validate_relative_path(release_license, "license inventory releaseLicenseFiles entry")
        if not release_license.startswith("LICENSES/"):
            raise PackagingError(
                "release license source must be under the repository LICENSES directory"
            )
        raw_files.append(
            {
                "source": release_license,
                "path": RELEASE_LICENSE_PREFIX + PurePosixPath(release_license).name,
                "mode": 0o644,
                "kind": (
                    "licenseInventory"
                    if release_license == license_inventory
                    else "licenseFile"
                ),
            }
        )

    payload: list[dict[str, Any]] = []
    destinations: set[str] = set()
    for index, raw in enumerate(raw_files):
        label = f"payload plan files[{index}]"
        if not isinstance(raw, dict):
            raise PackagingError(f"{label} must be an object")
        source_relative = raw.get("source")
        destination = raw.get("path")
        if not isinstance(source_relative, str):
            raise PackagingError(f"{label}.source must be a string")
        if not isinstance(destination, str):
            raise PackagingError(f"{label}.path must be a string")
        destination = validate_runtime_install_path(destination, f"{label}.path")
        if destination in destinations:
            raise PackagingError(f"duplicate archive path: {destination}")
        destinations.add(destination)
        source_path = ensure_regular_source(repo_root, source_relative, f"{label}.source")
        mode = validate_mode(raw.get("mode"), f"{label}.mode")
        payload.append(
            {
                "source": source_path,
                "sourceRelative": source_relative,
                "path": destination,
                "mode": mode,
                "size": source_path.stat().st_size,
                "sha256": sha256_file(source_path),
                "sha1": sha1_file(source_path),
                "kind": raw.get("kind"),
            }
        )

    if len(payload) > MAXIMUM_FILES:
        raise PackagingError(f"payload exceeds the {MAXIMUM_FILES}-file host limit")
    expanded_size = sum(entry["size"] for entry in payload)
    if expanded_size > MAXIMUM_ARCHIVE_BYTES:
        raise PackagingError("payload exceeds the 512 MiB expanded host limit")

    for file_entry in payload:
        path = file_entry["path"]
        for other in destinations:
            if other != path and other.startswith(path + "/"):
                raise PackagingError(
                    f"archive file path is also used as a directory: {path}"
                )

    missing_prefixes = [
        prefix
        for prefix in REQUIRED_RUNTIME_PREFIXES
        if not any(entry["path"].startswith(prefix) for entry in payload)
    ]
    if missing_prefixes:
        raise PackagingError(
            "payload omits required NativePipeRuntime layout prefix(es): "
            + ", ".join(missing_prefixes)
        )
    payload_by_path = {entry["path"]: entry for entry in payload}
    required = required_runtime_files(architecture)
    missing_runtime_files = sorted(set(required) - set(payload_by_path))
    if missing_runtime_files:
        raise PackagingError(
            "payload omits required dual-libc runtime file(s): "
            + ", ".join(missing_runtime_files)
        )
    for path, expected_mode in required.items():
        if payload_by_path[path]["mode"] != expected_mode:
            raise PackagingError(
                f"runtime file {path} must use mode {expected_mode}"
            )
    session_path = f"guest/session/dist/nativepipe-session-{architecture}"
    expected_session_source = f"nativepipe-session-{architecture}-musl"
    if PurePosixPath(payload_by_path[session_path]["sourceRelative"]).name != expected_session_source:
        raise PackagingError(
            "generic session binary must be sourced from the musl artifact "
            f"{expected_session_source}"
        )
    return sorted(payload, key=lambda entry: entry["path"])


def archive_directories(payload: Iterable[dict[str, Any]]) -> list[str]:
    directories: set[str] = set()
    for entry in payload:
        parent = PurePosixPath(entry["path"]).parent
        while str(parent) != ".":
            directories.add(str(parent))
            parent = parent.parent
    return sorted(directories)


def tar_info(name: str, mode: int, epoch: int, is_directory: bool) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name=name)
    info.mode = mode
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = epoch
    info.devmajor = 0
    info.devminor = 0
    if is_directory:
        info.type = tarfile.DIRTYPE
        info.size = 0
    else:
        info.type = tarfile.REGTYPE
    return info


def build_ustar(path: Path, payload: list[dict[str, Any]], epoch: int) -> None:
    entries: list[tuple[str, bool, dict[str, Any] | None]] = [
        (directory, True, None) for directory in archive_directories(payload)
    ] + [(entry["path"], False, entry) for entry in payload]
    entries.sort(key=lambda entry: entry[0])

    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    os.close(fd)
    temporary_path = Path(temporary)
    try:
        try:
            with tarfile.open(temporary_path, mode="w", format=tarfile.USTAR_FORMAT) as archive:
                for name, is_directory, payload_entry in entries:
                    if is_directory:
                        archive.addfile(tar_info(name, 0o755, epoch, True))
                        continue
                    assert payload_entry is not None
                    info = tar_info(name, payload_entry["mode"], epoch, False)
                    info.size = payload_entry["size"]
                    with payload_entry["source"].open("rb") as stream:
                        archive.addfile(info, stream)
        except (OSError, tarfile.TarError, ValueError) as error:
            raise PackagingError(f"cannot create USTAR archive: {error}") from error
        os.chmod(temporary_path, 0o644)
        os.replace(temporary_path, path)
    finally:
        temporary_path.unlink(missing_ok=True)


def build_spdx(
    version: str,
    release_tag: str,
    architecture: str,
    payload: list[dict[str, Any]],
    source_repository: str,
    source_commit: str,
    epoch: int,
) -> dict[str, Any]:
    namespace_seed = canonical_json_bytes(
        {
            "component": COMPONENT,
            "version": version,
            "architecture": architecture,
            "sourceRepository": source_repository,
            "sourceCommit": source_commit,
        }
    )
    namespace_digest = hashlib.sha256(namespace_seed).hexdigest()
    files: list[dict[str, Any]] = []
    relationships: list[dict[str, str]] = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": "SPDXRef-Package-nativepipe-runtime",
        }
    ]
    for index, entry in enumerate(payload):
        spdx_id = f"SPDXRef-File-{index + 1}"
        file_license = spdx_license_for_path(entry["path"])
        files.append(
            {
                "SPDXID": spdx_id,
                "fileName": "./" + entry["path"],
                "checksums": [
                    {"algorithm": "SHA1", "checksumValue": entry["sha1"]},
                    {"algorithm": "SHA256", "checksumValue": entry["sha256"]},
                ],
                "copyrightText": spdx_copyright_for_path(entry["path"]),
                "licenseConcluded": file_license,
                "licenseInfoInFiles": [file_license],
            }
        )
        relationships.append(
            {
                "spdxElementId": "SPDXRef-Package-nativepipe-runtime",
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": spdx_id,
            }
        )
    verification_code = hashlib.sha1(
        "".join(sorted(entry["sha1"] for entry in payload)).encode("ascii")
    ).hexdigest()
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"{COMPONENT}-{version}-{architecture}",
        "documentNamespace": f"urn:lighthouse:spdx:{namespace_digest}",
        "creationInfo": {
            "created": iso8601_utc(epoch),
            "creators": ["Tool: NativePipe deterministic runtime packager"],
        },
        "documentDescribes": ["SPDXRef-Package-nativepipe-runtime"],
        "packages": [
            {
                "SPDXID": "SPDXRef-Package-nativepipe-runtime",
                "name": COMPONENT,
                "versionInfo": version,
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": True,
                "licenseConcluded": PACKAGE_LICENSE,
                "licenseDeclared": PACKAGE_LICENSE,
                "copyrightText": "Copyright holders identified in the bundled license inventory and notices",
                "packageVerificationCode": {
                    "packageVerificationCodeValue": verification_code
                },
                "externalRefs": [
                    {
                        "referenceCategory": "OTHER",
                        "referenceType": "lighthouse-release-tag",
                        "referenceLocator": release_tag,
                    }
                ],
            }
        ],
        "hasExtractedLicensingInfos": [
            {
                "licenseId": ORIGINAL_LICENSE,
                "name": "NativePipe original source rights notice",
                "extractedText": (
                    "Copyright remains with the NativePipe project authors. "
                    "No open-source grant is inferred; consult the bundled source inventory."
                ),
            },
            {
                "licenseId": XWAYLAND_DEPENDENCY_LICENSE,
                "name": "Locked xwayland-satellite third-party dependency notices",
                "extractedText": (
                    "The complete dependency inventory, declared SPDX expressions, package-local "
                    "license and notice texts, Rust standard-library notices, the embedded Open "
                    "Sans attribution, and the wl_drm protocol notice are bundled next to each "
                    "xwayland-satellite runtime binary."
                ),
            },
        ],
        "files": files,
        "relationships": relationships,
    }


def validate_release_inputs(args: argparse.Namespace) -> None:
    if not VERSION_RE.fullmatch(args.version):
        raise PackagingError(f"invalid semantic version: {args.version!r}")
    expected_tag = f"nativepipe-runtime-v{args.version}-{args.release_sequence}"
    if args.release_tag != expected_tag:
        raise PackagingError(
            f"release tag must be exactly {expected_tag!r}, got {args.release_tag!r}"
        )
    if (
        type(args.release_sequence) is not int
        or args.release_sequence <= 0
        or args.release_sequence > MAXIMUM_RELEASE_SEQUENCE
    ):
        raise PackagingError("release sequence must be a positive UInt64 integer")
    if not REPOSITORY_RE.fullmatch(args.source_repository):
        raise PackagingError("source repository must use the exact owner/repository form")
    if not COMMIT_RE.fullmatch(args.source_commit):
        raise PackagingError(
            "source commit must be a lowercase 40-character hexadecimal ID"
        )
    if args.source_date_epoch < 0:
        raise PackagingError("SOURCE_DATE_EPOCH must be non-negative")
    if not args.builder_id or any(character in args.builder_id for character in "\r\n\x00"):
        raise PackagingError("builder ID must be a non-empty single-line value")


def run_license_inventory_check(repo_root: Path, requested_script: Path | None) -> None:
    script = requested_script or (repo_root / "scripts/check-license-inventory.py")
    if not script.is_absolute():
        script = repo_root / script
    try:
        metadata = script.lstat()
    except FileNotFoundError as error:
        raise PackagingError(f"license inventory checker is missing: {script}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise PackagingError(f"license inventory checker must be a regular file: {script}")
    completed = subprocess.run(
        [sys.executable, str(script)],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if completed.returncode != 0:
        detail = completed.stdout.strip() or f"exit status {completed.returncode}"
        raise PackagingError(f"license inventory check failed: {detail}")


def package_release(args: argparse.Namespace) -> dict[str, Path]:
    validate_release_inputs(args)
    repo_root = args.repo_root.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    if not repo_root.is_dir():
        raise PackagingError(f"repository root is not a directory: {repo_root}")
    run_license_inventory_check(repo_root, args.license_check_script)

    plan_path = args.payload_plan
    if not plan_path.is_absolute():
        plan_path = repo_root / plan_path
    tests_input_path = args.tests_input
    if not tests_input_path.is_absolute():
        tests_input_path = repo_root / tests_input_path
    tests_results = read_json(tests_input_path, "test results")
    if not isinstance(tests_results, dict):
        raise PackagingError("test results must be a JSON object")
    if tests_results.get("status") != "passed":
        raise PackagingError("test results status must be exactly 'passed'")

    payload = load_payload(
        repo_root,
        plan_path,
        args.license_inventory,
        args.architecture,
    )
    base_name = f"{COMPONENT}-{args.architecture}"
    archive_path = output_dir / f"{base_name}.tar"
    build_ustar(archive_path, payload, args.source_date_epoch)
    if archive_path.stat().st_size > MAXIMUM_ARCHIVE_BYTES:
        raise PackagingError("authoritative archive exceeds the 512 MiB host limit")

    outputs: dict[str, Path] = {"archive": archive_path}

    tests_path = output_dir / f"{base_name}.tests.json"
    write_json(
        tests_path,
        {
            "schemaVersion": SCHEMA_VERSION,
            "component": COMPONENT,
            "version": args.version,
            "sourceCommit": args.source_commit.lower(),
            "architecture": args.architecture,
            "results": tests_results,
        },
    )
    outputs["tests"] = tests_path

    license_reference = None
    license_entry = next(
        entry for entry in payload if entry.get("kind") == "licenseInventory"
    )
    license_reference = {
        "path": license_entry["path"],
        "size": license_entry["size"],
        "sha256": license_entry["sha256"],
    }
    source_path = output_dir / f"{base_name}.source.json"
    source_metadata: dict[str, Any] = {
        "schemaVersion": SCHEMA_VERSION,
        "component": COMPONENT,
        "sourceRepository": args.source_repository,
        "sourceCommit": args.source_commit,
        "version": args.version,
        "releaseTag": args.release_tag,
        "releaseSequence": args.release_sequence,
        "architecture": args.architecture,
        "sourceDateEpoch": args.source_date_epoch,
        "payloadPlanSha256": sha256_file(plan_path),
    }
    source_metadata["licenseInventory"] = license_reference
    write_json(source_path, source_metadata)
    outputs["source"] = source_path

    sbom_path = output_dir / f"{base_name}.spdx.json"
    write_json(
        sbom_path,
        build_spdx(
            args.version,
            args.release_tag,
            args.architecture,
            payload,
            args.source_repository,
            args.source_commit.lower(),
            args.source_date_epoch,
        ),
    )
    outputs["sbom"] = sbom_path

    archive_reference = metadata_reference(archive_path)
    provenance_path = output_dir / f"{base_name}.provenance.json"
    write_json(
        provenance_path,
        {
            "_type": "https://in-toto.io/Statement/v1",
            "subject": [
                {
                    "name": archive_path.name,
                    "digest": {"sha256": archive_reference["sha256"]},
                }
            ],
            "predicateType": "https://slsa.dev/provenance/v1",
            "predicate": {
                "buildDefinition": {
                    "buildType": "urn:nativepipe:build:runtime:v1",
                    "externalParameters": {
                        "releaseTag": args.release_tag,
                        "releaseSequence": args.release_sequence,
                        "architecture": args.architecture,
                    },
                    "resolvedDependencies": [
                        {
                            "uri": args.source_repository,
                            "digest": {"gitCommit": args.source_commit.lower()},
                        }
                    ],
                },
                "runDetails": {
                    "builder": {"id": args.builder_id},
                },
            },
        },
    )
    outputs["provenance"] = provenance_path

    manifest_path = output_dir / f"{base_name}.runtime-manifest.json"
    manifest: dict[str, Any] = {
        "schemaVersion": SCHEMA_VERSION,
        "component": COMPONENT,
        "guestRuntimeABI": GUEST_RUNTIME_ABI,
        "version": args.version,
        "releaseTag": args.release_tag,
        "releaseSequence": args.release_sequence,
        "sourceRepository": args.source_repository,
        "sourceCommit": args.source_commit.lower(),
        "architecture": args.architecture,
        "archive": archive_reference,
        "files": [
            {
                "path": entry["path"],
                "size": entry["size"],
                "sha256": entry["sha256"],
                "mode": entry["mode"],
            }
            for entry in payload
        ],
    }
    manifest_data = canonical_json_bytes(manifest)
    if len(manifest_data) > MAXIMUM_MANIFEST_BYTES:
        raise PackagingError("runtime manifest exceeds the one MiB host limit")
    write_bytes(manifest_path, manifest_data)
    outputs["manifest"] = manifest_path
    return outputs


def parser() -> argparse.ArgumentParser:
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument("--repo-root", type=Path, required=True)
    argument_parser.add_argument("--payload-plan", type=Path, required=True)
    argument_parser.add_argument("--tests-input", type=Path, required=True)
    argument_parser.add_argument("--output-dir", type=Path, required=True)
    argument_parser.add_argument("--version", required=True)
    argument_parser.add_argument("--release-tag", required=True)
    argument_parser.add_argument("--release-sequence", type=int, required=True)
    argument_parser.add_argument(
        "--architecture", choices=("aarch64", "x86_64"), required=True
    )
    argument_parser.add_argument("--source-repository", required=True)
    argument_parser.add_argument("--source-commit", required=True)
    argument_parser.add_argument("--source-date-epoch", type=int, required=True)
    argument_parser.add_argument("--builder-id", required=True)
    argument_parser.add_argument(
        "--license-inventory",
        default=LICENSE_INVENTORY_SOURCE,
        help=(
            "repository-relative inventory whose releaseLicenseFiles are included "
            "without reinterpreting their license claims"
        ),
    )
    argument_parser.add_argument(
        "--license-check-script",
        type=Path,
        help=(
            "checker to run before packaging (defaults to "
            "REPO/scripts/check-license-inventory.py)"
        ),
    )
    return argument_parser


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        outputs = package_release(args)
    except PackagingError as error:
        print(f"error: {error}", file=os.sys.stderr)
        return 2
    for label in sorted(outputs):
        print(f"{label}={outputs[label]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
