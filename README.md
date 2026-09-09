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

For NativePipe, one immutable media resource represents each committed frame.
The SSH stream carries binary NPIP window/scene messages and NPEN H.264 frames
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
nativepipe user@linux-host firefox --no-remote
nativepipe --compositor /home/user/bin/nativepipe-wayland user@linux-host gtk4-demo
nativepipe -i ~/.ssh/id_ed25519 -p 2222 user@linux-host qterminal
nativepipe --install-compositor user@linux-host gtk4-demo
```

Options before the destination configure SSH/NativePipe. Everything after it
is the target application's argument vector, not a login shell. The command
starts a dedicated compositor and exits when that command ends. Each invocation
has a private temporary Wayland runtime directory.

The system `/usr/bin/ssh -T` owns authentication, host-key checks, encryption,
and transport. No libssh, local listener, port forwarding, or lane-pairing nonce
is used. NPIP control/scene packets and NPEN media frames share stdout, with
their existing magic and lengths distinguishing complete frames. Host commands
travel over stdin; diagnostics and child application output go to stderr.
The bounded output writer never waits for SSH on the Wayland input event loop.

If the compositor is missing, NativePipe explains how to install it.
`--install-compositor` downloads the architecture/libc-specific GitHub Release,
checks its published SHA-256 digest, and installs it under
`~/.local/share/nativepipe/compositor` on the remote computer. No root access is
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

## Linux Actions artifacts and releases

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

Tags matching `nativepipe-v*` additionally create normal GitHub Release
archives for both architectures, plus four standalone remote compositor bundles.
The release contains `SHA256SUMS`, its
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
