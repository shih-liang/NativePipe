#!/usr/bin/env python3
"""Check the issued FSKit grants before signing or publishing a native package."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile

# Apple's TN3127 identifies Developer ID Application by these certificate
# extensions within the Apple anchor, rather than a user-editable name.
DEVELOPER_ID_REQUIREMENT = (
    "anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists "
    "and certificate leaf[field.1.2.840.113635.100.6.1.13] exists"
)
ROLES = {
    "NATIVEPIPE_PROVISIONING_PROFILE": ("com.nativepipe.cli", "mount", ""),
    "NATIVEPIPE_FILESYSTEM_PROVISIONING_PROFILE": (
        "com.nativepipe.cli.filesystem", "fsmodule", "Contents/Extensions/NativePipeFileSystemExtension.appex"),
}


def fail(field, identifier, reason):
    raise SystemExit(f"FSKit signing: {field} for {identifier}: {reason}")


def validate_profile(profile, identifier, capability, certificate, field="profile", mode="development"):
    if not isinstance(profile, dict) or not isinstance(profile.get("Entitlements"), dict):
        fail(field, identifier, "profile is not a valid entitlement record")
    grant = profile["Entitlements"]
    teams = profile.get("TeamIdentifier", [])
    if not isinstance(teams, list) or len(teams) != 1 or not isinstance(teams[0], str) or not re.fullmatch(r"[A-Z0-9]{10}", teams[0]):
        fail(field, identifier, "missing or ambiguous development team")
    application_identifier = grant.get("com.apple.application-identifier", "")
    prefixes = profile.get("ApplicationIdentifierPrefix", teams)
    if not isinstance(prefixes, list):
        fail(field, identifier, "missing application identifier prefix")
    if application_identifier not in [prefix + "." + identifier for prefix in prefixes if isinstance(prefix, str)]:
        fail(field, identifier, "profile does not authorize this explicit App ID")
    if grant.get("com.apple.developer.team-identifier") != teams[0]:
        fail(field, identifier, "profile entitlement does not match its development team")
    entitlement = "com.apple.developer.fskit." + capability
    if grant.get(entitlement) is not True:
        fail(field, identifier, "profile must authorize " + entitlement)
    if profile.get("Platform") != ["OSX"]:
        fail(field, identifier, "profile is not for macOS")
    expiration = profile.get("ExpirationDate")
    if not isinstance(expiration, datetime.datetime):
        fail(field, identifier, "profile has no expiration date")
    if expiration.tzinfo is None:
        expiration = expiration.replace(tzinfo=datetime.timezone.utc)
    if expiration <= datetime.datetime.now(datetime.timezone.utc):
        fail(field, identifier, "profile is expired")
    if not any(isinstance(data, bytes) and hashlib.sha1(data).hexdigest() == certificate
               for data in profile.get("DeveloperCertificates", [])):
        fail(field, identifier, "signing certificate is not authorized by this profile")
    if mode == "distribution" and (profile.get("ProvisionsAllDevices") is not True or
                                    grant.get("com.apple.security.get-task-allow") is True):
        fail(field, identifier, "public releases require an all-devices Developer ID profile without debugging access")
    return {"team": teams[0], "applicationIdentifier": application_identifier,
            "debuggable": grant.get("com.apple.security.get-task-allow") is True}


def read_profile(path, field, identifier):
    if not path.is_file():
        fail(field, identifier, "provide an issued provisioning profile; development and distribution both require it")
    result = subprocess.run(["security", "cms", "-D", "-i", str(path)], capture_output=True)
    try:
        return plistlib.loads(result.stdout) if result.returncode == 0 else fail(field, identifier, "cannot decode the CMS profile")
    except (ValueError, plistlib.InvalidFileException):
        fail(field, identifier, "cannot decode the CMS profile")


def signing_identity():
    identity = os.environ.get("SIGN_ID", "Developer ID Application")
    result = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"], capture_output=True, text=True)
    matches = [match.group(1).lower() for line in result.stdout.splitlines()
               if (match := re.search(r"\b([0-9A-Fa-f]{40})\b", line)) and
               (identity in line or identity.lower() == match.group(1).lower())]
    if result.returncode != 0 or len(matches) != 1:
        raise SystemExit("FSKit signing: SIGN_ID is unavailable or ambiguous; select one authorized certificate name or SHA-1")
    return matches[0]


def artifact_certificate(bundle, field, identifier):
    with tempfile.TemporaryDirectory(prefix="nativepipe-fskit-certificate-") as temporary:
        prefix = Path(temporary) / "certificate"
        result = subprocess.run(["codesign", "-d", "--extract-certificates=" + str(prefix), str(bundle)], capture_output=True)
        certificate = Path(str(prefix) + "0")
        if result.returncode != 0 or not certificate.is_file():
            fail(field, identifier, "the artifact has no signing certificate; ad-hoc signing cannot authorize FSKit")
        return hashlib.sha1(certificate.read_bytes()).hexdigest()


def verify_developer_id_identity(signer):
    # Validate the actual selected certificate, without relying on its display
    # name or parsing X.509 in Python. This owned, never-executed copy has no
    # FSKit entitlements and is removed before returning.
    with tempfile.TemporaryDirectory(prefix="nativepipe-signing-identity-") as temporary:
        probe = Path(temporary) / "signing-certificate-check"
        shutil.copyfile("/usr/bin/true", probe)
        probe.chmod(0o755)
        signed = subprocess.run(["codesign", "--force", "--timestamp=none", "--sign", signer, str(probe)], capture_output=True)
        checked = subprocess.run(["codesign", "--verify", "--strict", "--test-requirement",
                                  DEVELOPER_ID_REQUIREMENT, str(probe)], capture_output=True) if signed.returncode == 0 else signed
        if checked.returncode != 0:
            raise SystemExit("FSKit signing: public releases require the selected Apple-issued Developer ID Application certificate")


def verify(app=None, mode="development"):
    if mode not in ("development", "distribution"):
        raise SystemExit("FSKit signing: NATIVEPIPE_PACKAGE_MODE must be development or distribution")
    paths = {}
    # Check all paths first, without prompting the keychain for a missing profile.
    for field, (identifier, _, relative) in ROLES.items():
        value = str(app / relative / "Contents/embedded.provisionprofile") if app else os.environ.get(field)
        if not value or not Path(value).is_file():
            fail(field, identifier, "provide an issued provisioning profile; development and distribution both require it")
        paths[field] = Path(value)
    decoded = {field: read_profile(paths[field], field, identifier)
               for field, (identifier, _, _) in ROLES.items()}
    identity = None if app else signing_identity()
    result = {}
    for field, (identifier, capability, relative) in ROLES.items():
        bundle = app / relative if app else None
        signer = artifact_certificate(bundle, field, identifier) if app else identity
        result[field] = validate_profile(decoded[field], identifier, capability, signer, field, mode)
        if app:
            signature = subprocess.run(["codesign", "-dvv", str(bundle)], check=True, capture_output=True, text=True).stderr
            if "TeamIdentifier=" + result[field]["team"] not in signature or "Identifier=" + identifier + "\n" not in signature:
                fail(field, identifier, "signature identity differs from its provisioning profile")
            entitlement_result = subprocess.run(["codesign", "-d", "--entitlements", ":-", str(bundle)],
                                               check=True, capture_output=True)
            signed = plistlib.loads(entitlement_result.stdout)
            if (signed.get("com.apple.application-identifier") != result[field]["applicationIdentifier"] or
                    signed.get("com.apple.developer.team-identifier") != result[field]["team"] or
                    signed.get("com.apple.developer.fskit." + capability) is not True):
                fail(field, identifier, "signed entitlements differ from profile authorization")
    if len({value["team"] for value in result.values()}) != 1:
        raise SystemExit("FSKit signing: caller and module profiles must belong to the same development team")
    if mode == "distribution":
        if app:
            for _, (_, _, relative) in ROLES.items():
                checked = subprocess.run(["codesign", "--verify", "--strict", "--test-requirement",
                                          DEVELOPER_ID_REQUIREMENT, str(app / relative)], capture_output=True)
                if checked.returncode != 0:
                    raise SystemExit("FSKit signing: public release artifact is not signed by an Apple-issued Developer ID Application certificate")
        else:
            verify_developer_id_identity(identity)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, help="verify embedded profiles using each artifact's public signing certificate")
    parser.add_argument("--mode", choices=["development", "distribution"], default=os.environ.get("NATIVEPIPE_PACKAGE_MODE", "development"))
    parser.add_argument("--json", action="store_true", help="emit signing identifiers for package entitlements")
    options = parser.parse_args()
    checked = verify(options.app, options.mode)
    print(json.dumps(checked) if options.json else "NativePipe FSKit caller and module profiles verified")
