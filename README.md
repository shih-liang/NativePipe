# NativePipe

NativePipe is both the standalone macOS remote-Linux client and the shared
display, input and transport technology used by FluxWindow. There is one
Wayland protocol and surface-state implementation in `guest/compositor`; two
concrete backends select how frames and events cross the machine boundary:

- `nativepipe-wayland` uses loopback TCP and H.264/alpha media resources for an
  SSH-forwarded remote Linux machine.
- `vmpipe-wayland` uses the FluxWindow vsock window channel and Venus/
  virtio-gpu resources inside a VM.

The standalone client and FluxWindow therefore share commit, damage, popup,
input, clipboard, drag-and-drop, scaling, presentation, frame-callback, and
Xwayland semantics.
They do not carry two copied compositor state machines.

Backend selection happens entirely at link time. Both implementations export
the same `np_backend_run` symbol, while the shared `main.c` only calls that
symbol. `vmpipe-wayland` links `backends/vmpipe`; `nativepipe-wayland` links
`backends/remote`. There is no runtime backend factory, command-line selector,
or `NP_REMOTE` conditional compilation in the compositor frontend. Transport
state, imported GPU resources, encoded media state and presentation holds are
opaque to the shared Wayland server.

## Display behavior

The shared compositor implements `xdg_toplevel`, `xdg_popup`, synchronized and
desynchronized subsurfaces, window geometry, pointer/keyboard/scroll/text
input, clipboard and Wayland drag-and-drop, decorations, viewport and output
scale, damage, presentation feedback, frame callbacks, and
`wp_fifo_manager_v1`.

Rootless X11 support is a pinned xwayland-satellite v0.8.2 source build. Its
upstream commit is recorded in `guest/xwayland-satellite/UPSTREAM_COMMIT`; the
checked-in patch only drops a pointer leave that has no corresponding forwarded
enter. The normal test target runs deterministic Rust library tests. The
separate `test-integration` target starts a real Xwayland server and belongs on
an isolated graphical test host.

For NativePipe, one immutable media resource represents each committed frame.
The SSH channels carry binary NPIP window/scene messages and NPEN H.264 frames
with optional alpha sidecars. Encoder backpressure keeps the newest
not-yet-encoded image, and a Wayland frame callback completes only after its
scene is latched by the macOS display clock.

For FluxWindow, the same scene state names guest-created GPU resources. The VM
backend receives host events over vsock and presents the compositor's
window-level resources without importing the remote encoder into the VM path.

## NativePipe client

Requirements are macOS 14 or newer and Xcode/Swift 6:

```sh
make nativepipe
make test
```

This produces the standalone `.build/release/nativepipe` command. It is never
copied into FluxWindow.app or the `nativepipe-runtime` guest archive.

`make test` covers the standalone protocol, windowing, and remote-client stack
without renderer SDKs. FluxWindow owns its VZ custom virtio-gpu device and the
virglrenderer, MoltenVK, and ANGLE/libepoxy integration because those components
are specific to its VM host process.

The command has no third-party Swift dependencies. VideoToolbox decodes into
IOSurface-backed BGRA frames and the shared Metal scene renderer composites the
same atomic layer snapshots used by FluxWindow.

Use it over SSH:

```sh
nativepipe user@linux-host
nativepipe user@linux-host --compositor ~/bin/nativepipe-wayland
nativepipe user@linux-host -i ~/.ssh/id_ed25519 -p 2222
```

The CLI starts or reuses one compositor owned by the remote user, opens local
SSH forwards, connects both binary channels, and opens a login shell with
`WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `DISPLAY`, and `XAUTHORITY` set. Runtime
state is private to that UID under `/tmp/nativepipe-xdg-$UID`.

For an existing manual tunnel:

```sh
nativepipe --host 127.0.0.1 --surface-port 1025 --media-port 1026
```

The remote compositor listens only on loopback. Port 1025 carries NPIP window
events, host commands, and scenes; port 1026 carries NPEN media frames. Each
connection begins with a 16-byte `NPRH` hello containing a random session token
and lane kind. The compositor pairs only equal-token surface/media lanes,
emits `channelReady` as the first NPIP frame, and opens media output afterward.
The client temporarily holds current-token media that wins the TCP race against
`channelReady`, so the initial IDR is not lost.

`NPRH` is a lane-correlation nonce, not an authentication credential. NativePipe
trusts every process running as the selected remote Unix account; the compositor
ports must remain loopback-only and be reached through the authenticated SSH
tunnel. Isolating mutually untrusted processes that share one Unix account would
require carrying both streams inside SSH-owned file descriptors instead of
publishing shared loopback listeners.

## Guest builds

Builds are native to their target architecture and libc. The release workflow
uses aarch64 and x86_64 runners with glibc and musl containers and records the
resolved builder-image digest and installed tool versions.

The important targets are:

```sh
make -C guest/compositor remote
make -C guest/compositor vmpipe
make -C guest/session dist-target
make -C guest/xwayland-satellite test
make -C guest/xwayland-satellite dist-target
```

The compositor needs Wayland, xkbcommon, and Vulkan headers. The remote backend
also needs EGL/GLES, GBM, DRM, and FFmpeg development packages. The session
helpers need DRM and Vulkan headers. xwayland-satellite fetches only its pinned
commit and builds with Cargo's checked-in lock file.

## Signed NativePipe runtime releases

A release tag is exactly:

```text
nativepipe-runtime-v<VERSION>-<positive releaseSequence>
```

`releaseSequence` is the positive generation number of the complete guest
runtime set. It must be larger than every previous display release, and a
publish operation must use the same value `G` for all three identities:

- the display manifest's `releaseSequence`
- the matching platform manifest's `releaseSequence`
- the FluxWindow baseline lock's `setGeneration`

This repository does not contact or coordinate with the platform repository.
The release operator assigns the shared generation, and the FluxWindow host
accepts a platform/display pair only when both signed manifests have the same
sequence as the lock generation. `setGeneration` is deliberately not another
display-manifest field; the manifest schema remains frozen. Each architecture
is one atomic runtime archive that contains both GNU and musl variants. The
current FluxWindow application selects `aarch64`; `x86_64` is published under
the same contract for future consumers.

The authoritative aarch64 assets are:

```text
nativepipe-runtime-aarch64.tar
nativepipe-runtime-aarch64.runtime-manifest.json
nativepipe-runtime-aarch64.runtime-manifest.sig
```

The x86_64 names replace `aarch64` with `x86_64`. The uncompressed archive is a
deterministic USTAR containing only files used by the guest runtime:

- `guest/session/dist/**` plus service, profile, and Vulkan-layer configuration
- `guest/xwayland-satellite/dist/**` plus `VERSION`
- `Packages/NativePipe/guest/compositor/dist/**` plus
  `COMPOSITOR_SOURCE.sha256`
- the release license inventory and required third-party license text under
  `LICENSES/nativepipe-runtime/`

Each shipped xwayland-satellite binary has a sibling
`*.third-party-licenses.txt`. Release CI derives it from the pinned upstream
`Cargo.lock`, traverses the non-dev normal/build dependency closure, rejects a
crate without a declared license, and bundles package-local notices together
with the exact Rust 1.89.0 distribution COPYRIGHT, Apache-2.0, and MIT texts.
It also verifies and attributes the embedded Open Sans font by its pinned
SHA-256 and extracts the MIT notice from the bundled `wl_drm` protocol XML.
The file is part of the signed runtime manifest, not an unsigned build log.

Source, tests, SBOM, and provenance stay in the public repository or separate
release assets; they are not expanded into `NativePipeRuntime`.

The canonical JSON manifest has exactly the frozen top-level fields
`schemaVersion`, `component`, `version`, `releaseTag`, `releaseSequence`,
`sourceRepository`, `sourceCommit`, `architecture`, `guestRuntimeABI`,
`archive`, and `files`. It uses schema version 1, component
`nativepipe-runtime`, guest runtime ABI 1, the owner/repository source slug,
source commit, architecture, release identity, archive size/hash, and every
runtime file's relative path, size, SHA-256, and integer mode (`420` or `493`).
It deliberately has neither `setGeneration` nor a libc subtarget because the
sequence is the set generation and both libc variants activate as a unit.

The tag commit must be reachable from protected `main` and must descend from
the commit named by the greatest earlier release sequence. A higher sequence
therefore cannot re-sign an older vulnerable source ancestor. Signing is delegated to
`.github/workflows/sign-nativepipe-runtime.yml@main`, so the key-bearing policy
does not come from the tag. That job has only `contents: read`, verifies the
unsigned artifacts, signs the exact canonical manifest bytes with Ed25519, and
passes the result through an immutable Actions artifact. A separate publisher
has `contents: write` but no private key and cryptographically verifies the
artifact, including an independent comparison of its public key with the
repository variable, before creating the release. The signer derives the raw
public key
from the protected private key and requires an exact match with the repository variable
`LIGHTHOUSE_RUNTIME_ED25519_PUBLIC_KEY_BASE64`; a missing or mismatched value
fails the release. `SHA256SUMS` and its signature are extra supply-chain
evidence, not an indirect substitute for the manifest signature. No private key
is generated or stored in this repository. The protected
`LIGHTHOUSE_RUNTIME_ED25519_PRIVATE_KEY_BASE64` secret is the base64 encoding of
an unencrypted PKCS#8 Ed25519 private-key PEM; the public repository variable
is the canonical base64 encoding of the corresponding 32 raw public-key bytes.
These checks rely on repository controls and fail closed when those controls
are absent. Configure a branch ruleset for `main` that blocks force-push and
deletion, requires pull-request review and required CI, and requires code-owner
review for `.github/workflows/**`, `scripts/package_nativepipe_runtime.py`,
`scripts/verify_nativepipe_runtime.py`, and `scripts/check-license-inventory.py`.
Configure a tag ruleset for `nativepipe-runtime-v*` that restricts creation to
the release operators and prohibits every update, force-update, and deletion;
also reserve the exact tag name `main`. The monotonic sequence and source-lineage
checks use those immutable tags as their high-water mark. The
`lighthouse-runtime-release` environment must require an independent reviewer,
disallow administrator bypass, and expose the signing secret only to the
protected release-tag deployment rule. The fixed public-key repository variable
must be changed only through the same reviewed release-key rotation procedure.

FluxWindow release builds vendor a complete platform+display runtime set using
their lock file. The lock pins each repository, public key, guest ABI, manifest,
signature, and archive, and its `setGeneration` equals both manifests'
`releaseSequence`. Runtime updates verify both signed manifests, reject mixed
sequences, stage the complete set in a new directory, and atomically promote it
only when no VM host is running. Activation is forward-only; a failed candidate
never replaces the active set.

## Source and license policy

`LICENSES/source-inventory.json` is authoritative for checked-in source origin;
the release archive carries it as
`LICENSES/nativepipe-runtime/source-inventory.json` to avoid collisions with
other runtime components.
The release workflow fails closed if a source file is uncovered, ambiguously
covered, or a required license text is absent. Wayland XML and generated
protocol sources retain their embedded MIT notices. xwayland-satellite and its
derivative patch retain MPL-2.0 and ship its complete license text. Its locked
Cargo dependencies and the statically linked Rust standard library are audited
during every target build; their deterministic inventory and notices ship next
to each binary, together with the embedded Open Sans and `wl_drm` notices. SPDX
file records use explicit path-derived license conclusions, and the
verifier/signing boundary rejects `NOASSERTION` for every packaged file.

The project-original and generated integration files are intentionally marked
`LicenseRef-NativePipe-Original`: publishing the repository does not silently
infer or grant an open-source license for those files. The copyright owner must
choose and add an explicit project license before describing NativePipe itself
as open source.
