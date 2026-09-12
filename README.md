# NativePipe

NativePipe is both the standalone macOS remote-Linux client and the shared
display, input and transport technology used by FluxWindow. There is one
Wayland protocol and surface-state implementation in `guest/compositor`; two
concrete backends select how frames and events cross the machine boundary:

- `nativepipe-wayland` uses SSH stdin/stdout and H.264/alpha media resources
  for a remote Linux machine.
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

For NativePipe, a scene and its encoded resources are published as one ordered
unit: metadata follows the images it references. Pending, unencoded updates
coalesce to the newest state; encoded H.264 reference frames are never dropped.
Unchanged alpha planes are reused explicitly within the same encoder epoch.
The remote virtual output grants Wayland frame/FIFO callbacks at its refresh
deadline only when transport and display capacity remain. Actual macOS
presentation separately controls admission; network latency is not imposed as
one stop-and-wait roundtrip per frame. The VM's display-clock pacing is unchanged.

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
nativepipe user@linux-host firefox --no-remote
nativepipe --compositor /home/user/bin/nativepipe-wayland user@linux-host gtk4-demo
nativepipe -i ~/.ssh/id_ed25519 -p 2222 user@linux-host qterminal
nativepipe --install-compositor user@linux-host gtk4-demo
```

Options before the destination configure SSH/NativePipe. Everything after it
is the target application's argument vector, not a login shell. The command
starts a dedicated compositor and exits when that command ends. Each invocation
has a private temporary Wayland runtime directory.

The system `/usr/bin/ssh -T -C` owns authentication, host-key checks, encryption,
and transport. No libssh, local listener, port forwarding, or lane-pairing nonce
is used. Short NPIP control records bypass credit-paced bulk records. Images,
scenes and large replies are wrapped in NPRF fragments (at most 16 KiB), inside
NPIP; display and background lanes preserve their own order.
Window lifecycle events stay ordered with scenes, and catalog end markers stay
behind their batches; only independent transactions may overtake these records.
NPRA acknowledges
received bytes, independently of decoding and display. Each sender starts with
4 KiB of credit and adapts between 1–64 KiB from measured acknowledgement delay.
It pauses admission when the receiver falls behind, instead of queueing more
frames in SSH. One latest unencoded scene and a per-window limit of 8 pending
presentations also bound encode/decode work. NPRP distinguishes actual drawable
presentation from supersession/cancellation; ordinary frame/FIFO acknowledgements
are not treated as proof that a scene was displayed. It also carries the window's
current CADisplayLink interval, so encoding follows the actual display cadence,
not the screen's advertised maximum rate. A zero Metal presented-time
is not counted as display. Occluded windows retain their unused display credits
until visible again, so a hidden animation stops encoding instead of cycling
through discard acknowledgements. This does not stop input or other windows.

Host commands travel over stdin; diagnostics and child output go to stderr.
The writer never waits for SSH on the Wayland input event loop. OpenSSH
compression covers every stream, including metadata, alpha, clipboard and SFTP;
H.264 is already compressed and has no additional application-level compressor.
Display and SFTP disable ControlMaster reuse so a file transfer does not share
the display's TCP queue. A single TCP stream still cannot bypass bytes already
sent or a lost TCP packet; bounded adaptive admission reduces that unavoidable
head-of-line delay, not the physical network RTT.

If the compositor is missing, NativePipe explains how to install it.
`--install-compositor` checks GitHub on each connection and downloads the
architecture/libc-specific release only when its published SHA-256 digest changes.
Verified bundles live under `~/.local/share/nativepipe/compositor/releases/<digest>`;
an atomic `current` link selects the completed installation. Running connections
keep their own immutable directory, including libraries loaded later. Explicit
`--compositor` paths bypass this installer. No root access is
used. FFmpeg/VA-API, GLib/GIO, EGL/GL/GBM/DRM and libc come from the Linux
distribution; they are not bundled or replaced. Image libraries and matching
GdkPixbuf PNG/XPM loaders are bundled without exporting LD_LIBRARY_PATH.

The remote encoder links the system `libavcodec`, `libavutil` and `libswscale`
libraries, not the `ffmpeg` command. It uses FFmpeg's VA-API H.264 encoder when
available and its software H.264 encoder otherwise. Installing libva alone
does not replace these FFmpeg dependencies. The target must supply the library
major versions named by the binary's `DT_NEEDED`; GNU/musl and CPU architecture
alone do not guarantee compatibility. If the distribution no longer supplies
those versions, build the compositor against that distribution's development
packages. Never symlink incompatible FFmpeg major versions. The launcher checks
runtime linking before starting the session and reports missing libraries.

OpenSSH can use its normal keys, config and agent. Passwords and encrypted-key
passphrases can be remembered in macOS Keychain by the askpass dialog; host-key
confirmation and one-time codes are never replayed as passwords.

## FluxWindow integration

The manager stores each remote computer alongside virtual machines and starts
one sandboxed `FluxWindowRemoteHost.app` per connection. The helper runs
`nativepipe-wayland --stdio --session` through the same SSH implementation,
holding multiple applications on one display. GIO supplies the installed
desktop application catalogue and launches desktop entries on that display.
The manager and FluxWindow Apps both browse that catalogue and raise the same
windows rather than spawning additional SSH sessions.

VMHost and RemoteHost share `WindowBridge`, the Dock window switcher, and VMHost's
appearance/input-source observers and preference resolution, now extracted into
`HostIntegrationController`. Command coalescing/writing and local helper socket
ownership are shared too. File drag and file clipboard use one AppKit bridge;
only the user-vsock versus SFTP transfer adapter differs.
The remote transport/codec remains separate from VZ and virtio-gpu resources.
SSH file access uses user-selected security-scoped bookmarks; passwords stay in
the app group's Keychain, not in the connection plist.

The remote compositor requires Linux 5.3 or later. Direct command sessions use
`pidfd` to observe the exact child process independently of Xwayland's signals.

## Guest builds

Builds are native to their target architecture and libc. The release workflow
uses aarch64 and x86_64 runners with glibc and musl containers. The FFmpeg
development packages are build dependencies only and are not copied into
remote release bundles, nor are their codec-only transitive dependencies.

The important targets are:

```sh
make -C guest/compositor remote
make -C guest/compositor vmpipe
make -C guest/session dist-target
```

The compositor needs Wayland, xkbcommon, Vulkan, GIO (including gio-unix),
GdkPixbuf, and librsvg development packages. Both backends use the same
[application discovery and launch worker](guest/compositor/APPLICATIONS.md).
The remote backend also needs EGL/GLES, GBM, DRM, and FFmpeg. The session
helpers need DRM and Vulkan headers. Rootless X11 additionally requires the
distribution packages `xwayland-satellite` and Xwayland at runtime. Remote
sessions require `dbus-run-session` to isolate application activation from the
remote machine's physical desktop session.

## Builds and releases

`.github/workflows/build-linux.yml` builds the VM compositor and session helpers
plus the remote compositor for aarch64/x86_64 and GNU/musl. Every successful run uploads four
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

Only an explicitly pushed `nativepipe-v*` version tag publishes a GitHub Release.
Ordinary commits run CI and build artifacts; manual workflow runs build and
validate the complete product set without publishing. The tag workflow builds
all three products from the same commit and publishes them together after tests:

| Product | Release archives |
| --- | --- |
| VM compositor and session helpers | `nativepipe-vm-compositor-{aarch64,x86_64}.tar.gz` (GNU and musl in each) |
| Remote compositor with private image libraries | `nativepipe-compositor-{aarch64,x86_64}-{gnu,musl}.tar.gz` |
| macOS `nativepipe` CLI | `nativepipe-macos-universal.tar.gz` (Apple Silicon and Intel, macOS 14+) |

There are seven product archives and three shared verification files:
`SHA256SUMS`, its Ed25519 signature, and the public key. Reports, provenance,
test results and intermediate build files stay in CI artifacts. License notices
remain inside the corresponding archives. VM archives exclude the remote binary.
The CLI is ad-hoc signed; it is not Apple Developer ID notarized.

The signer has no repository write permission. A separate publisher verifies
the complete product set, executable modes, digests and signature before creating
the release. A missing product prevents publication. Remote clients select a
stable release containing compositor assets, so older VM-runtime-only releases
cannot be mistaken for an installable remote bundle.

## Source and license policy

`LICENSES/source-inventory.json` records checked-in source origins. Wayland XML
and generated protocol sources retain their embedded MIT notices.
The guest distribution, rather than NativePipe, distributes and updates
xwayland-satellite and its license notices.

Project-original integration files remain
`LicenseRef-NativePipe-Original`: publishing the repository does not itself
infer or grant an open-source license for those files.
