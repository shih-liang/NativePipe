# RemotePipe

RemotePipe presents applications from a real Linux machine as native macOS
windows. It is the standalone remote counterpart to LightHouse's VM display
path: the Wayland state machine and AppKit window semantics are the same, while
pixels travel over SSH as H.264 instead of virtio-gpu resources.

## What works

- `xdg_toplevel`, `xdg_popup`, synchronized/desynchronized subsurfaces and
  window geometry
- native pointer, keyboard, scroll, cursor-shape and text-input routing
- clipboard and Wayland drag-and-drop
- client/server decorations, viewport, integer/fractional scale, output state,
  damage, frame callbacks and `wp_fifo_manager_v1`
- rootless Xwayland (`DISPLAY` and `XAUTHORITY` are exported automatically)
- `wl_shm` and single-plane ARGB/XRGB linux-dmabuf v4 feedback, including
  device-native modifiers for GPU-accelerated Xwayland
- one immutable media resource per committed frame, window-level scene
  snapshots, decoder-safe reconnect and alpha sidecars for cursors, drag icons
  and translucent subsurfaces

RemotePipe intentionally does not contain LightHouse VM hosting, virtio-gpu,
Venus, VGL or blob-alignment code. Explicit DRM syncobj is not advertised on a
remote machine; implicit dma-buf fences are observed before the encoder reads a
buffer. Linear images are mapped directly. Device-native tiled images are
imported through the matching EGL render node and converted to packed BGRA.

The compositor selects the first accessible DRM render node for dmabuf
feedback. Set `REMOTEPIPE_RENDER_NODE=/dev/dri/renderD…` when a multi-GPU host
needs a specific device.

## macOS client

Requirements: macOS 14 or newer and Xcode/Swift 6.

```sh
swift build -c release --product remotepipe
```

The command has no third-party Swift dependencies. VideoToolbox decodes into
IOSurface-backed BGRA frames and the shared Metal scene renderer composites the
same layer snapshots used by the VM path.

## Linux compositor

Install a C compiler plus development packages for Wayland, wayland-protocols,
cJSON, xkbcommon, XCB/XComposite and FFmpeg (`libavcodec`, `libavutil`,
`libswscale`). For example on Alpine:

```sh
doas apk add build-base linux-headers wayland-dev wayland-protocols \
  cjson-dev libxkbcommon-dev libxcb-dev mesa-dev libdrm-dev ffmpeg-dev
make -C guest/compositor
make -C guest/compositor print-binary
```

On Debian/Ubuntu the corresponding packages are `build-essential`,
`libwayland-dev`, `wayland-protocols`, `libcjson-dev`, `libxkbcommon-dev`,
`libxcb1-dev`, `libxcb-composite0-dev`, `libegl1-mesa-dev`,
`libgles2-mesa-dev`, `libgbm-dev`, `libdrm-dev`, `libavcodec-dev`,
`libavutil-dev` and `libswscale-dev`.

`make install` installs `remotepipe-wayland` under `/usr/local/bin` by default.
The compositor listens only on remote loopback:

| Port | Payload |
|---|---|
| 1025 | NPIP window events, host commands and binary scene snapshots |
| 1026 | NPEN H.264 frames and PackBits alpha sidecars |

## Use over SSH

```sh
remotepipe user@linux-host
remotepipe user@linux-host --compositor ~/bin/remotepipe-wayland
remotepipe user@linux-host -i ~/.ssh/id_ed25519 -p 2222
```

The CLI starts or reuses one compositor owned by the remote user, opens local
SSH forwards, connects both channels, then opens a login shell with
`WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `DISPLAY` and `XAUTHORITY` set. Runtime
state is private to that UID under:

```text
/tmp/remotepipe-xdg-$UID/
  remotepipe-wayland.pid
  remotepipe-wayland.log
  remotepipe-wayland.env
```

The PID and both listening ports must agree before an existing compositor is
reused; an unrelated process occupying 1025/1026 is never adopted.

For an existing manual tunnel, connect without SSH orchestration:

```sh
remotepipe --host 127.0.0.1 --surface-port 1025 --media-port 1026
```

## Display pipeline

```text
Linux client wl_buffer
  -> RemotePipe Wayland commit/state validation
  -> per-surface H.264 + optional lossless alpha
  -> SSH TCP forwards
  -> VideoToolbox / IOSurface
  -> one atomic window scene
  -> Metal / NSWindow
```

Encoder backpressure keeps only the newest not-yet-encoded image. A scene names
that image with a unique resource ID, so an old swapchain buffer, a replacement
frame and a reconnect cannot alias each other. Wayland frame callbacks are
completed only after the corresponding scene is latched by the macOS display
clock.
