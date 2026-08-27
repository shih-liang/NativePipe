# NativePipe

Wayland ↔ macOS display bridge. Local LightHouse VMs and remote real-machine
hosts both use this package; neither path requires the other.

Shared features (window protocol, AppKit bridge, Wayland compositor core) are
written **once**. VM and Remote only swap transport (vsock/virtio vs TCP/H.264).

## Contents

| Path | Role |
|---|---|
| `Sources/NativePipeProtocol` | NPIP framing, window events/commands, ports, NPEN |
| `Sources/NativePipeWindowing` | `WindowBridge` / `NativeWindow` → `NSWindow` |
| `Sources/NativePipeGPU` | Host virtio-gpu (`VZCustomVirtioDevice`) — built only by the root LightHouse package |
| `Sources/NativePipeVenus` | VMHost-only C bridge — built only by the root LightHouse package |
| `Sources/NativePipeRemote` | TCP client, VideoToolbox decode, `DisplaySession` — **Remote** |
| `Sources/NativePipeRemoteApp` | `remotepipe` CLI (SSH only; display via NativePipeRemote) |
| `guest/compositor` | Shared compositor core + thin VM/Remote mains |
| `guest/encoder` | H.264 encode (VAAPI preferred, libx264 fallback) — **Remote** |

## Standalone use

```bash
cd Packages/NativePipe
swift build -c release --product remotepipe
```

## Linux compositor — compile-time split

| Target | Binary | Role |
|---|---|---|
| `make` | **`.build/<arch>-<libc>/vmpipe-wayland`** | LightHouse **VM**: vsock + virtio blobs / VirGL + Venus |
| `make remote` | **`.build/<arch>-<libc>/remotepipe-wayland`** | **Bare metal**: TCP `1025`/`1026` + H.264 |

Shared code: `compositor.c`, `hostlink`, Wayland protocols.  
VM-only objects: `blob.c`, `dmabuf.c`, `vmpipe_main.c`.  
Remote-only objects: `medialink.c`, `encoder`, `remotepipe_main.c` (no virtio).

The standalone package intentionally exposes only protocol, windowing, and
remote targets so it keeps its macOS 14 deployment contract. VM GPU targets use
macOS 27 Virtualization APIs and external renderer SDKs, and are owned by the
root LightHouse build.

### VM guest

```bash
cd guest/compositor
apk add build-base linux-headers wayland-dev wayland-protocols cjson-dev \
        libxkbcommon-dev
make
make -s print-binary
# → .../guest/compositor/.build/aarch64-musl/vmpipe-wayland
```

### Remote bare-metal host

```bash
cd guest/compositor
apk add build-base linux-headers wayland-dev wayland-protocols cjson-dev \
        libxkbcommon-dev ffmpeg-dev
make remote
# → .build/aarch64-musl/remotepipe-wayland
```

| Port | Content |
|---|---|
| **1025** | NPIP window metadata (`GuestEvent` / `HostCommand`) |
| **1026** | NPEN H.264 Annex-B frames (remote only) |

## One-shot SSH session (recommended)

With `remotepipe-wayland` installed on the Linux host:

```bash
remotepipe user@host
remotepipe user@host --compositor ~/nativepipe-guest/compositor/.build/aarch64-gnu/remotepipe-wayland
remotepipe user@host -i ~/.ssh/id_ed25519 -p 2222
```

What it does (SSH in the CLI; display via `NativePipeRemote.DisplaySession`):

1. Local port forwards to remote 1025/1026 (`ssh -N`).
2. Starts `remotepipe-wayland` if needed under isolated
   `XDG_RUNTIME_DIR=/tmp/remotepipe-xdg-$UID` (avoids colliding with a session
   compositor’s `wayland-0`). Env written to `/tmp/remotepipe-wayland.env`.
3. Connects the macOS display client.
4. Interactive remote shell with `WAYLAND_DISPLAY` set (real Terminal TTY).
5. `exit` tears down the local client and tunnel; remote compositor stays up.

## Manual three-terminal setup (debugging)

```bash
# Terminal A — Mac
ssh -N -L 1025:127.0.0.1:1025 -L 1026:127.0.0.1:1026 user@linux

# Terminal B — Linux
./.build/aarch64-gnu/remotepipe-wayland

# Terminal C — Mac
remotepipe --host 127.0.0.1
```

## Adding features once (VM + Remote)

Put transport-agnostic work in:

- `NativePipeProtocol` — events / ports
- `NativePipeWindowing` — AppKit UX
- `guest/compositor/compositor.c` (+ shared helpers) — Wayland side

Do **not** put shared features only in `remotepipe` CLI, `blob`/`dmabuf`, or
`encoder`/`medialink`.

Requirements:

- Remote encodes with FFmpeg (`h264_vaapi` when available, else `libx264`).
- macOS decodes with VideoToolbox.
- MVP remote path encodes **wl_shm** clients.
