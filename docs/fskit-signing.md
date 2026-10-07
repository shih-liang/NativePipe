# Signing NativePipe file sharing

NativePipe shares selected Linux files through its bundled Apple FSKit module.
Publishing a file records its metadata. Finder and Mac applications request the
byte ranges they actually read; disconnecting revokes those file capabilities.
The display command supports macOS 14 or later, while this native file sharing
requires macOS 27 or later. Build both targets with the macOS 27 SDK.

The shared libraries are `NativePipeFileSharing` and `NativePipeFileSystem`.
NativePipe and LinPortal each package their own extension identity, avoiding
registration conflicts when both products are installed.

## Apple authorization

An ad-hoc signature or a free Personal Team cannot provision FSKit Module.
Apple's [macOS capability table](https://developer.apple.com/help/account/reference/supported-capabilities-macos/)
lists FSKit Module for Apple Developer Program and Developer ID teams. A valid
code-signing certificate alone is insufficient: macOS can terminate an executable
before `main` if its FSKit entitlement has no matching issued profile.

Register these explicit App IDs in the same paid developer team:

| App ID | Required entitlement | Package variable |
| --- | --- | --- |
| `com.nativepipe.cli` | `com.apple.developer.fskit.mount` | `NATIVEPIPE_PROVISIONING_PROFILE` |
| `com.nativepipe.cli.filesystem` | `com.apple.developer.fskit.fsmodule` | `NATIVEPIPE_FILESYSTEM_PROVISIONING_PROFILE` |

The module also uses App Sandbox and outgoing local-socket access. It receives
the selected directory grant through a security-scoped FSPathURLResource; it
requires no App Group, VM or network-server entitlement. The packager derives
the team and application identifier prefixes from the issued profiles.

1. Register the App IDs in [Certificates, Identifiers & Profiles](https://developer.apple.com/account/resources/identifiers/list)
   and enable **FSKit Module** for the extension.
2. For local testing, create matching [Mac App Development profiles](https://developer.apple.com/help/account/provisioning-profiles/create-a-development-provisioning-profile/)
   for an authorized Apple Development certificate and test Mac. For releases
   outside the App Store, use the corresponding Developer ID profiles and a
   Developer ID Application certificate.
3. Check that the issued caller profile includes `com.apple.developer.fskit.mount`.
   The macOS 27 SDK documents this entitlement, but a matching portal capability
   name has not been verified here. If Apple cannot issue this grant through the
   portal, contact [Apple Developer Support](https://developer.apple.com/contact/).
   Editing an entitlement file does not grant it.

## Package the universal command

Build `nativepipe` and `NativePipeFileSystemExtension` for arm64 and x86_64, then
combine each pair with `lipo`. Keep `NativePipe_NativePipeStrings.bundle` beside
the universal command. Package with the two issued profiles:

```sh
SIGN_ID='Developer ID Application: Your Organization (TEAMID)' \
NATIVEPIPE_PROVISIONING_PROFILE='/path/NativePipe.provisionprofile' \
NATIVEPIPE_FILESYSTEM_PROVISIONING_PROFILE='/path/NativePipeFileSystem.provisionprofile' \
sh scripts/package-macos-cli.sh \
  .build/universal/nativepipe macos-release \
  .build/universal/NativePipeFileSystemExtension
```

For development, select the corresponding Apple Development identity/profiles;
`NATIVEPIPE_PACKAGE_MODE=development` is the local default. Public release jobs
set `NATIVEPIPE_PACKAGE_MODE=distribution`, which requires all-devices profiles
without debugging access and verifies the actual selected Developer ID Application
certificate using Apple's [code-signing requirement](https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements).
Certificate display names are not treated as proof of certificate type.
The packager rejects missing grants, mismatched App IDs or teams, expired
profiles, and unauthorized signing certificates before replacing an existing
release archive. It embeds both profiles and signs the extension before the
application. Resource/version checks do not mutate or ad-hoc re-sign signed code.

`scripts/verify-macos-package.py NativePipe.app` checks the native extension,
universal binaries, signatures, embedded profiles, translations, version, and
weak FSKit linkage on macOS. The portable release-archive verifier checks only
archive structure and integrity; it does not prove CMS/signature validity.

## GitHub release secrets

The release signing job uses:

- `APPLE_CODESIGN_CERTIFICATE_BASE64`: the exported signing certificate and
  private key as a base64-encoded `.p12` file.
- `APPLE_CODESIGN_CERTIFICATE_PASSWORD`: its export password.
- `APPLE_CODESIGN_IDENTITY`: the exact authorized certificate name or SHA-1.
- `NATIVEPIPE_PROVISIONING_PROFILE_BASE64`: the issued caller profile.
- `NATIVEPIPE_FILESYSTEM_PROVISIONING_PROFILE_BASE64`: the issued module profile.

Every commit can produce a profile-free build artifact for testing. Tagged
release jobs consume those build artifacts and require the signing secrets above.
Ordinary build artifacts are not installed or published as signed releases.
Missing release signing inputs must fail the release job. Notarization, if
required for distribution, is a separate step; signing alone does not claim it.

## Enable and validate

Keep `NativePipe.app` alongside the `bin` directory when installing the archive.
Open `NativePipe.app` once so macOS can discover its extension, then enable
**NativePipe Shared Files** in **System Settings → General → Login Items &
Extensions → File System Extensions**. Apple's [FSKit sample](https://developer.apple.com/documentation/fskit/building-a-passthrough-file-system)
describes this extension toggle; it is separate from profile authorization.

The current local tests exercise real Unix-socket range reads, catalog revocation,
and native FSKit adapters without a mounted volume. Actual Finder mounting,
extension discovery and busy-volume eject still require an authorized macOS 27
installation and have not been established by these unit tests.
