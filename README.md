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

Rootless X11 uses the guest distribution's `xwayland-satellite` and Xwayland
packages. NativePipe neither fetches nor builds that Rust project. The
compositor finds `xwayland-satellite` through the session `PATH`, starts it only
when an X11 client connects, and leaves X11 disabled when the package is absent.

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
```

The compositor needs Wayland, xkbcommon, and Vulkan headers. The remote backend
also needs EGL/GLES, GBM, DRM, and FFmpeg development packages. The session
helpers need DRM and Vulkan headers. Rootless X11 additionally requires the
distribution packages `xwayland-satellite` and Xwayland at runtime.

## Linux Actions artifacts and releases

`.github/workflows/build-linux.yml` builds the VM compositor and session helpers
for aarch64/x86_64 and GNU/musl. Every successful run uploads four
checkout-shaped artifacts:

```text
nativepipe-linux-aarch64-gnu
nativepipe-linux-aarch64-musl
nativepipe-linux-x86_64-gnu
nativepipe-linux-x86_64-musl
```

Downloading one of these artifacts at the repository root restores its files
under the ordinary `guest/**/dist` paths. FluxWindow uses the artifacts from
the run for the NativePipe checkout commit; it does not build Linux binaries on
the Mac and it does not use a runtime lock, manifest, or release archive.

Tags matching `nativepipe-v*` additionally create normal GitHub Release
archives for both architectures. The release contains `SHA256SUMS`, its
Ed25519 signature, and the matching public key. Signing runs without repository
write permission; a separate job verifies the digest and signature before
publishing the release.

## Source and license policy

`LICENSES/source-inventory.json` records checked-in source origins. Wayland XML
and generated protocol sources retain their embedded MIT notices.
The guest distribution, rather than NativePipe, distributes and updates
xwayland-satellite and its license notices.

Project-original integration files remain
`LicenseRef-NativePipe-Original`: publishing the repository does not itself
infer or grant an open-source license for those files.
