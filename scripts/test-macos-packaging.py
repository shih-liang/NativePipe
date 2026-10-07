#!/usr/bin/env python3
"""Validate FSKit signing rejection and preserve outputs without fake signatures."""
import copy
import datetime
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
script = source / "scripts/verify-fskit-profiles.py"
spec = importlib.util.spec_from_file_location("nativepipe_profiles", script)
profiles = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profiles)

# These dictionaries test entitlement policy only. They are not CMS files,
# signing certificates, valid Apple profiles or evidence of a native mount.
certificate = b"profile-policy test data"
signer = hashlib.sha1(certificate).hexdigest()
identifier, capability = "com.nativepipe.cli", "mount"
profile = {"TeamIdentifier": ["ABCDEFGHIJ"], "ApplicationIdentifierPrefix": ["ABCDEFGHIJ"],
           "Platform": ["OSX"], "ExpirationDate": datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(days=1),
           "DeveloperCertificates": [certificate],
           "Entitlements": {"com.apple.application-identifier": "ABCDEFGHIJ." + identifier,
                            "com.apple.developer.team-identifier": "ABCDEFGHIJ",
                            "com.apple.developer.fskit.mount": True}}
assert profiles.validate_profile(profile, identifier, capability, signer)["team"] == "ABCDEFGHIJ"
for key, value in (("com.apple.application-identifier", "ABCDEFGHIJ.com.nativepipe.*"),
                   ("com.apple.developer.team-identifier", "OTHERTEAM1"),
                   ("com.apple.developer.fskit.mount", False)):
    invalid = copy.deepcopy(profile)
    invalid["Entitlements"][key] = value
    try:
        profiles.validate_profile(invalid, identifier, capability, signer)
    except SystemExit:
        pass
    else:
        raise AssertionError("Invalid entitlement accepted: " + key)
for key, value in (("ExpirationDate", datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=1)),
                   ("Platform", ["iOS"]), ("DeveloperCertificates", [b"different certificate"]),
                   ("TeamIdentifier", ["SHORT"]), ("Entitlements", [])):
    invalid = copy.deepcopy(profile)
    invalid[key] = value
    try:
        profiles.validate_profile(invalid, identifier, capability, signer)
    except SystemExit:
        pass
    else:
        raise AssertionError("Invalid profile accepted: " + key)

try:
    profiles.validate_profile(profile, identifier, capability, signer, mode="distribution")
except SystemExit as error:
    assert "all-devices Developer ID profile" in str(error)
else:
    raise AssertionError("Device-bound development profile accepted as a public release")
all_devices = copy.deepcopy(profile)
all_devices["ProvisionsAllDevices"] = True
all_devices["Entitlements"]["com.apple.security.get-task-allow"] = True
try:
    profiles.validate_profile(all_devices, identifier, capability, signer, mode="distribution")
except SystemExit:
    pass
else:
    raise AssertionError("Debuggable profile accepted as a public release")

with tempfile.TemporaryDirectory(prefix="nativepipe-signing-rejection-") as temporary:
    root = Path(temporary)
    output = root / "existing-output"
    output.mkdir()
    archive = output / "nativepipe-macos-universal.tar.gz"
    archive.write_bytes(b"preserve the previous valid release")
    environment = os.environ.copy()
    for field in (*profiles.ROLES, "SIGN_ID", "NATIVEPIPE_PACKAGE_MODE"):
        environment.pop(field, None)
    rejected = subprocess.run(["sh", "scripts/package-macos-cli.sh", str(root / "unused-binary"), str(output)],
                              cwd=source, env=environment, capture_output=True, text=True)
    assert rejected.returncode != 0 and "provide an issued provisioning profile" in rejected.stderr, rejected.stderr
    assert archive.read_bytes() == b"preserve the previous valid release"
    # Even with both filenames present, ordinary plists/text must not be passed
    # through as issued CMS profiles. No signing or filesystem mounting occurs.
    for field in profiles.ROLES:
        invalid = root / (field + ".provisionprofile")
        invalid.write_bytes(b"invalid CMS content")
        environment[field] = str(invalid)
    rejected = subprocess.run(["sh", "scripts/package-macos-cli.sh", str(root / "unused-binary"), str(output)],
                              cwd=source, env=environment, capture_output=True, text=True)
    assert rejected.returncode != 0 and "cannot decode the CMS profile" in rejected.stderr, rejected.stderr
    assert archive.read_bytes() == b"preserve the previous valid release"
print("PASS explicit App ID, team, FSKit grant, platform, expiration and certificate and public-release profile policy; missing/invalid CMS rejection preserves release output")
