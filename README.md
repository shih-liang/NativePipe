# NativePipe

Wayland ↔ macOS display bridge. Local LightHouse VMs and future remote
real-machine hosts both use this package; neither path requires the other.

## Contents

| Path | Role |
|---|---|
| `Sources/NativePipeProtocol` | NPIP framing, window events/commands, ports |
| `Sources/NativePipeWindowing` | `WindowBridge` / `NativeWindow` → `NSWindow` |
| `Sources/NativePipeGPU` | Host virtio-gpu (`VZCustomVirtioDevice`) |
| `Sources/NativePipeVenus` | C bridge: dlopen virglrenderer / Venus host |
| `guest/compositor` | Guest Wayland compositor (`nativepipe-wayland`) |

## Standalone use

This directory is a complete Swift package. Remote macOS hosts that only need
the display bridge can depend on it alone:

```swift
.package(path: "…/Packages/NativePipe")
```

The LightHouse repo root also compiles these targets (via `path:`) because
the working tree is named `NativePipe`, and a nested `.package(path:)` would
collide with the root package identity on case-insensitive APFS.

## Guest compositor

Build inside the Linux guest:

```bash
cd NativePipe/guest/compositor
apk add build-base linux-headers wayland-dev wayland-protocols cjson-dev libxkbcommon-dev
make
```
