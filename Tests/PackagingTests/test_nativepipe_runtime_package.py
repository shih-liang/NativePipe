#!/usr/bin/env python3
"""Offline regression tests for deterministic runtime packaging."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
PACKAGER = REPOSITORY / "scripts" / "package_nativepipe_runtime.py"
VERIFIER = REPOSITORY / "scripts" / "verify_nativepipe_runtime.py"
COMMIT = "0123456789abcdef0123456789abcdef01234567"


def openssl_executable() -> str:
    configured = os.environ.get("OPENSSL")
    if configured:
        return configured
    for candidate in (
        "/opt/homebrew/opt/openssl@3/bin/openssl",
        "/usr/local/opt/openssl@3/bin/openssl",
    ):
        if Path(candidate).is_file():
            return candidate
    return shutil.which("openssl") or "openssl"


class RuntimePackageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "repo"
        self.root.mkdir()
        files = {"build/nativepipe-session-aarch64-musl": b"session\x00binary\n"}
        for libc in ("gnu", "musl"):
            files.update(
                {
                    f"build/nativepipe-align-blob-aarch64-{libc}.so": f"align-{libc}\n".encode(),
                    f"build/nativepipe-vulkan-layer-aarch64-{libc}.so": f"layer-{libc}\n".encode(),
                    f"build/xwayland-satellite-aarch64-{libc}": f"xwayland-{libc}\n".encode(),
                    f"build/xwayland-satellite-aarch64-{libc}.third-party-licenses.txt": (
                        f"locked Cargo and Rust license notice {libc}\n"
                    ).encode(),
                    f"build/vmpipe-wayland-aarch64-{libc}": f"compositor-{libc}\n".encode(),
                }
            )
        for relative, contents in files.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        fixture_licenses = self.root / "LICENSES"
        fixture_licenses.mkdir()
        for name in (
            "source-inventory.json",
            "OFL-1.1.txt",
            "xwayland-satellite-MPL-2.0.txt",
        ):
            (fixture_licenses / name).write_bytes((REPOSITORY / "LICENSES" / name).read_bytes())
        self.plan = self.root / "payload.json"
        plan_files = [
            {
                "source": "build/nativepipe-session-aarch64-musl",
                "path": "guest/session/dist/nativepipe-session-aarch64",
                "mode": 493,
            }
        ]
        for libc in ("gnu", "musl"):
            plan_files.extend(
                [
                    {
                        "source": f"build/nativepipe-align-blob-aarch64-{libc}.so",
                        "path": f"guest/session/dist/nativepipe-align-blob-aarch64-{libc}.so",
                        "mode": 420,
                    },
                    {
                        "source": f"build/nativepipe-vulkan-layer-aarch64-{libc}.so",
                        "path": f"guest/session/dist/nativepipe-vulkan-layer-aarch64-{libc}.so",
                        "mode": 420,
                    },
                    {
                        "source": f"build/xwayland-satellite-aarch64-{libc}",
                        "path": f"guest/xwayland-satellite/dist/xwayland-satellite-aarch64-{libc}",
                        "mode": 493,
                    },
                    {
                        "source": f"build/xwayland-satellite-aarch64-{libc}.third-party-licenses.txt",
                        "path": f"guest/xwayland-satellite/dist/xwayland-satellite-aarch64-{libc}.third-party-licenses.txt",
                        "mode": 420,
                    },
                    {
                        "source": f"build/vmpipe-wayland-aarch64-{libc}",
                        "path": f"Packages/NativePipe/guest/compositor/dist/vmpipe-wayland-aarch64-{libc}",
                        "mode": 493,
                    },
                ]
            )
        self.plan.write_text(
            json.dumps({"schemaVersion": 1, "files": plan_files}, sort_keys=True)
            + "\n",
            encoding="utf-8",
        )
        self.tests_input = self.root / "test-results.json"
        self.tests_input.write_text(
            '{"commands":[{"name":"unit","status":"passed"}],"status":"passed"}\n',
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def package_command(self, output: Path, plan: Path | None = None) -> list[str]:
        return [
            sys.executable,
            str(PACKAGER),
            "--repo-root",
            str(self.root),
            "--payload-plan",
            str(plan or self.plan),
            "--tests-input",
            str(self.tests_input),
            "--output-dir",
            str(output),
            "--version",
            "1.2.3",
            "--release-tag",
            "nativepipe-runtime-v1.2.3-1002003",
            "--release-sequence",
            "1002003",
            "--architecture",
            "aarch64",
            "--source-repository",
            "example/RemotePipe",
            "--source-commit",
            COMMIT,
            "--source-date-epoch",
            "1700000000",
            "--builder-id",
            "https://github.com/example/RemotePipe/actions/workflows/release.yml",
            "--license-check-script",
            str(REPOSITORY / "scripts/check-license-inventory.py"),
        ]

    def run_package(self, output: Path, plan: Path | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            self.package_command(output, plan),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def manifest(self, output: Path) -> Path:
        return output / "nativepipe-runtime-aarch64.runtime-manifest.json"

    def test_reproducible_ustar_and_fixed_sibling_metadata(self) -> None:
        output_a = Path(self.temporary.name) / "a"
        output_b = Path(self.temporary.name) / "b"
        first = self.run_package(output_a)
        second = self.run_package(output_b)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        names_a = sorted(path.name for path in output_a.iterdir())
        names_b = sorted(path.name for path in output_b.iterdir())
        self.assertEqual(names_a, names_b)
        self.assertEqual(
            names_a,
            [
                "nativepipe-runtime-aarch64.provenance.json",
                "nativepipe-runtime-aarch64.runtime-manifest.json",
                "nativepipe-runtime-aarch64.source.json",
                "nativepipe-runtime-aarch64.spdx.json",
                "nativepipe-runtime-aarch64.tar",
                "nativepipe-runtime-aarch64.tests.json",
            ],
        )
        for name in names_a:
            self.assertEqual(
                hashlib.sha256((output_a / name).read_bytes()).hexdigest(),
                hashlib.sha256((output_b / name).read_bytes()).hexdigest(),
                name,
            )
        verified = subprocess.run(
            [sys.executable, str(VERIFIER), str(self.manifest(output_a))],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(verified.returncode, 0, verified.stderr)

    def test_layout_modes_and_archive_hash(self) -> None:
        output = Path(self.temporary.name) / "output"
        result = self.run_package(output)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads(self.manifest(output).read_text(encoding="utf-8"))
        self.assertEqual(
            set(manifest),
            {
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
            },
        )
        self.assertEqual(manifest["releaseTag"], "nativepipe-runtime-v1.2.3-1002003")
        self.assertEqual(manifest["sourceRepository"], "example/RemotePipe")
        self.assertEqual(manifest["architecture"], "aarch64")
        self.assertEqual(manifest["guestRuntimeABI"], 1)
        self.assertIs(type(manifest["releaseSequence"]), int)
        self.assertTrue(all(type(entry["mode"]) is int for entry in manifest["files"]))
        archive = output / manifest["archive"]["name"]
        self.assertEqual(manifest["archive"]["size"], archive.stat().st_size)
        self.assertEqual(
            manifest["archive"]["sha256"], hashlib.sha256(archive.read_bytes()).hexdigest()
        )
        with tarfile.open(archive, "r:") as runtime:
            file_names = {member.name for member in runtime.getmembers() if member.isfile()}
        self.assertIn("guest/session/dist/nativepipe-session-aarch64", file_names)
        self.assertIn(
            "guest/xwayland-satellite/dist/xwayland-satellite-aarch64-gnu",
            file_names,
        )
        self.assertIn(
            "Packages/NativePipe/guest/compositor/dist/vmpipe-wayland-aarch64-gnu",
            file_names,
        )
        for libc in ("gnu", "musl"):
            self.assertIn(
                f"guest/session/dist/nativepipe-align-blob-aarch64-{libc}.so",
                file_names,
            )
            self.assertIn(
                f"guest/session/dist/nativepipe-vulkan-layer-aarch64-{libc}.so",
                file_names,
            )
            self.assertIn(
                f"guest/xwayland-satellite/dist/xwayland-satellite-aarch64-{libc}",
                file_names,
            )
            self.assertIn(
                f"guest/xwayland-satellite/dist/xwayland-satellite-aarch64-{libc}.third-party-licenses.txt",
                file_names,
            )
            self.assertIn(
                f"Packages/NativePipe/guest/compositor/dist/vmpipe-wayland-aarch64-{libc}",
                file_names,
            )
        self.assertIn(
            "LICENSES/nativepipe-runtime/source-inventory.json", file_names
        )
        self.assertIn(
            "LICENSES/nativepipe-runtime/OFL-1.1.txt",
            file_names,
        )
        self.assertIn(
            "LICENSES/nativepipe-runtime/xwayland-satellite-MPL-2.0.txt",
            file_names,
        )
        self.assertNotIn("LICENSES/source-inventory.json", file_names)
        self.assertFalse(
            any(PurePosixPath(name).name.startswith("nativepipe-wayland") for name in file_names),
            "the standalone remote compositor must not enter FluxWindow's runtime archive",
        )
        self.assertNotIn("nativepipe", {PurePosixPath(name).name for name in file_names})

    def test_release_sequence_is_atomic_set_generation_without_schema_extension(self) -> None:
        """The platform release and LightHouse lock must independently use this value."""
        output = Path(self.temporary.name) / "set-generation"
        result = self.run_package(output)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads(self.manifest(output).read_text(encoding="utf-8"))

        # Host activation requires platform.releaseSequence ==
        # display.releaseSequence == GuestRuntime.lock.json.setGeneration.
        # Cross-repository coordination belongs to the release operator and the
        # host; this display manifest must retain its frozen 11-field schema.
        self.assertEqual(manifest["releaseSequence"], 1002003)
        self.assertEqual(manifest["releaseTag"], "nativepipe-runtime-v1.2.3-1002003")
        self.assertNotIn("setGeneration", manifest)
        self.assertEqual(len(manifest), 11)

    def test_spdx_resolves_every_file_license_and_rejects_noassertion(self) -> None:
        output = Path(self.temporary.name) / "spdx-licenses"
        result = self.run_package(output)
        self.assertEqual(result.returncode, 0, result.stderr)
        sbom_path = output / "nativepipe-runtime-aarch64.spdx.json"
        sbom = json.loads(sbom_path.read_text(encoding="utf-8"))
        self.assertTrue(sbom["files"])
        for entry in sbom["files"]:
            self.assertNotEqual(entry["licenseConcluded"], "NOASSERTION")
            self.assertNotIn("NOASSERTION", entry["licenseInfoInFiles"])
            self.assertNotEqual(entry["copyrightText"], "NOASSERTION")

        sbom["files"][0]["licenseConcluded"] = "NOASSERTION"
        sbom["files"][0]["licenseInfoInFiles"] = ["NOASSERTION"]
        sbom_path.write_text(
            json.dumps(sbom, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        rejected = subprocess.run(
            [sys.executable, str(VERIFIER), str(self.manifest(output))],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("SPDX file license is unresolved", rejected.stderr)

    def test_rejects_unsafe_destination_and_non_integer_mode(self) -> None:
        for name, mutation in (
            ("unsafe", {"path": "../escape"}),
            ("string-mode", {"mode": "0755"}),
            ("unsupported-mode", {"mode": 448}),
        ):
            plan_value = json.loads(self.plan.read_text(encoding="utf-8"))
            plan_value["files"][0].update(mutation)
            plan = self.root / f"{name}.json"
            plan.write_text(json.dumps(plan_value), encoding="utf-8")
            result = self.run_package(Path(self.temporary.name) / name, plan)
            self.assertNotEqual(result.returncode, 0)

    def test_rejects_symlink_source(self) -> None:
        symlink = self.root / "build/symlink-session"
        try:
            symlink.symlink_to("nativepipe-session-aarch64-musl")
        except (OSError, NotImplementedError):
            self.skipTest("symbolic links are unavailable")
        plan_value = json.loads(self.plan.read_text(encoding="utf-8"))
        plan_value["files"][0]["source"] = "build/symlink-session"
        plan = self.root / "symlink.json"
        plan.write_text(json.dumps(plan_value), encoding="utf-8")
        result = self.run_package(Path(self.temporary.name) / "symlink", plan)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symbolic link", result.stderr)

    def test_rejects_non_regular_source_and_failed_license_check(self) -> None:
        plan_value = json.loads(self.plan.read_text(encoding="utf-8"))
        plan_value["files"][0]["source"] = "build"
        plan = self.root / "directory-source.json"
        plan.write_text(json.dumps(plan_value), encoding="utf-8")
        result = self.run_package(Path(self.temporary.name) / "directory", plan)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("regular file", result.stderr)

        failed_checker = self.root / "failed-check.py"
        failed_checker.write_text(
            "raise SystemExit('inventory intentionally rejected')\n", encoding="utf-8"
        )
        command = self.package_command(Path(self.temporary.name) / "failed-check")
        checker_index = command.index("--license-check-script") + 1
        command[checker_index] = str(failed_checker)
        checked = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(checked.returncode, 0)
        self.assertIn("license inventory check failed", checked.stderr)

    def test_rejects_tag_sequence_and_repository_contract_violations(self) -> None:
        cases = (
            ("tag", "--release-tag", "nativepipe-runtime-v1.2.3-7"),
            ("sequence", "--release-sequence", "0"),
            ("repository", "--source-repository", "https://github.com/example/RemotePipe"),
        )
        for name, flag, replacement in cases:
            command = self.package_command(Path(self.temporary.name) / name)
            command[command.index(flag) + 1] = replacement
            result = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0, name)

        prerelease = self.package_command(Path(self.temporary.name) / "prerelease")
        prerelease[prerelease.index("--version") + 1] = "1.2.3-rc.1"
        prerelease[prerelease.index("--release-tag") + 1] = (
            "nativepipe-runtime-v1.2.3-rc.1-1002003"
        )
        accepted = subprocess.run(
            prerelease,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(accepted.returncode, 0, accepted.stderr)

    def test_rejects_missing_musl_and_extra_manifest_field(self) -> None:
        plan_value = json.loads(self.plan.read_text(encoding="utf-8"))
        plan_value["files"] = [
            entry
            for entry in plan_value["files"]
            if entry["path"]
            != "Packages/NativePipe/guest/compositor/dist/vmpipe-wayland-aarch64-musl"
        ]
        plan = self.root / "missing-musl.json"
        plan.write_text(json.dumps(plan_value), encoding="utf-8")
        missing = self.run_package(Path(self.temporary.name) / "missing-musl", plan)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("dual-libc", missing.stderr)

        output = Path(self.temporary.name) / "extra-field"
        result = self.run_package(output)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest_value = json.loads(self.manifest(output).read_text(encoding="utf-8"))
        manifest_value["signature"] = {"file": "forbidden"}
        self.manifest(output).write_text(
            json.dumps(manifest_value, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        verified = subprocess.run(
            [sys.executable, str(VERIFIER), str(self.manifest(output))],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("field set mismatch", verified.stderr)

    def test_verifier_cryptographically_verifies_manifest_signature(self) -> None:
        output = Path(self.temporary.name) / "fixed-names"
        result = self.run_package(output)
        self.assertEqual(result.returncode, 0, result.stderr)
        openssl = openssl_executable()
        verifier_environment = {**os.environ, "OPENSSL": openssl}
        private_key = Path(self.temporary.name) / "ed25519-private.pem"
        public_key = Path(self.temporary.name) / "ed25519-public.pem"
        generated = subprocess.run(
            [openssl, "genpkey", "-algorithm", "ED25519", "-out", str(private_key)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(generated.returncode, 0, generated.stderr)
        exported = subprocess.run(
            [
                openssl,
                "pkey",
                "-in",
                str(private_key),
                "-pubout",
                "-out",
                str(public_key),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(exported.returncode, 0, exported.stderr)
        unsigned = subprocess.run(
            [
                sys.executable,
                str(VERIFIER),
                str(self.manifest(output)),
                "--require-signature",
                str(public_key),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(unsigned.returncode, 0)
        signature = output / "nativepipe-runtime-aarch64.runtime-manifest.sig"
        signed_manifest = subprocess.run(
            [
                openssl,
                "pkeyutl",
                "-sign",
                "-rawin",
                "-inkey",
                str(private_key),
                "-in",
                str(self.manifest(output)),
                "-out",
                str(signature),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(signed_manifest.returncode, 0, signed_manifest.stderr)
        signed = subprocess.run(
            [
                sys.executable,
                str(VERIFIER),
                str(self.manifest(output)),
                "--require-signature",
                str(public_key),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            env=verifier_environment,
        )
        self.assertEqual(signed.returncode, 0, signed.stderr)

        original_manifest = self.manifest(output).read_bytes()
        manifest_value = json.loads(original_manifest)
        manifest_value["sourceRepository"] = "example/Tampered"
        self.manifest(output).write_text(
            json.dumps(manifest_value, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        tampered_manifest = subprocess.run(
            [
                sys.executable,
                str(VERIFIER),
                str(self.manifest(output)),
                "--require-signature",
                str(public_key),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            env=verifier_environment,
        )
        self.assertNotEqual(tampered_manifest.returncode, 0)
        self.assertIn("signature verification failed", tampered_manifest.stderr)

        self.manifest(output).write_bytes(original_manifest)
        damaged_signature = bytearray(signature.read_bytes())
        damaged_signature[0] ^= 0x01
        signature.write_bytes(damaged_signature)
        tampered_signature = subprocess.run(
            [
                sys.executable,
                str(VERIFIER),
                str(self.manifest(output)),
                "--require-signature",
                str(public_key),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            env=verifier_environment,
        )
        self.assertNotEqual(tampered_signature.returncode, 0)
        self.assertIn("signature verification failed", tampered_signature.stderr)

        wrong_name = output / "renamed.runtime-manifest.json"
        wrong_name.write_bytes(self.manifest(output).read_bytes())
        renamed = subprocess.run(
            [sys.executable, str(VERIFIER), str(wrong_name)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(renamed.returncode, 0)

    def test_verifier_detects_archive_corruption(self) -> None:
        output = Path(self.temporary.name) / "corrupt"
        result = self.run_package(output)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads(self.manifest(output).read_text(encoding="utf-8"))
        archive = output / manifest["archive"]["name"]
        contents = bytearray(archive.read_bytes())
        contents[1024] ^= 0x01
        archive.write_bytes(contents)
        verified = subprocess.run(
            [sys.executable, str(VERIFIER), str(self.manifest(output))],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("SHA-256 mismatch", verified.stderr)


if __name__ == "__main__":
    unittest.main()
