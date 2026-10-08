# Wayland frontend

NativePipe shares one Wayland frontend between the VM and SSH backends. Protocol
state belongs to the frontend; buffer import, transport and GPU synchronization
belong to the backend. macOS window state and input remain in NativePipeWindowing.

## Advertised interfaces

| Interface | Version | Backend |
| --- | --- | --- |
| `wl_compositor` | 6 | Both |
| `wl_subcompositor` | 1 | Both |
| `wl_data_device_manager` | 4 | Both; source/device/offer children remain v3 |
| `xdg_wm_base` | 6 | Both |
| `wl_seat` | 9 | Both; pointer and keyboard only |
| `wl_shm` | 2 | Both; managed by pinned libwayland |
| `wl_output` | 4 | Both |
| `wp_viewporter` | 1 | Both |
| `wp_fractional_scale_manager_v1` | 1 | Both |
| `wp_cursor_shape_manager_v1` | 1 | Both |
| `wp_fifo_manager_v1` | 1 | Both |
| `zwp_text_input_manager_v3` | 1 | Both |
| `zxdg_decoration_manager_v1` | 1 | Both |
| `xdg_activation_v1` | 1 | Both |
| `zxdg_output_manager_v1` | 3 | Both |
| `wp_presentation` | 1 | Both, after initial host clock calibration |
| `zwp_linux_dmabuf_v1` | 4 | VM; SSH chooses v3/v4 according to its importer |
| `wp_linux_drm_syncobj_manager_v1` | 1 | VM, only after runtime timeline/eventfd checks |

The host currently supplies integer AppKit backing scales. Fractional-scale
events therefore carry those values multiplied by 120; the compositor does not
invent 1.25x or 1.5x preferences. Viewporter handles client source/destination
mapping. Existing surfaces and newly attached subsurface trees follow their
parent window's output and backing scale.

`xdg_activation_v1` accepts recent delivered input serials, uses expiring one-use
tokens and revokes them on focus changes, unmapping, destruction and disconnect.
The host independently checks its recent input and active origin window. Native
application-catalog launches retain their existing activation behavior.

## Presentation and buffer lifetime

Buffer-read completion, FIFO/frame callbacks and actual display presentation are
separate events. A drawable callback cannot replace the buffer's read hold or
its explicit-sync release condition. Superseding a queued scene still permits
the newest scene to render without releasing buffers being read by Metal.

VM and SSH share an internal drawable-outcome journal and clock-calibration
path. Each scene carries the compositor nonce, clock epoch and guest send time;
the host samples receipt before UI delivery. A scene keeps its historical clock
anchor. Feedback records move to a server-owned ledger after transport admission, so
surface destruction does not erase a known display result.

Scene admission is separate from connection liveness. Once the backend accepts
a scene, a subsequent transport failure must not release its pixel-read hold as
if it had never been sent. A rejected scene removes only its unsubmitted ledger
placeholder and can retry under the current clock epoch.

The host records the drawable result on Metal's callback thread. Hide, capture,
read completion and transport-credit return cannot override a submitted result.
Unacknowledged positive and proven pre-submission discard results survive a
transport disconnect and replay only to the same compositor nonce. The journal
stores numeric metadata, without retaining drawables or source textures.

VM pause closes scene submission, fences the guest display stream, waits for
submitted drawable results and asks the guest to acknowledge its feedback drain.
Resume establishes a fresh clock epoch before drawing reopens. A failed barrier
fails the pause operation; it does not manufacture a discarded frame. Missing
submitted outcomes have a finite recovery deadline. If actual public feedback
cannot be recovered, the affected Wayland client receives an implementation
error rather than a false display/discard event or endless retries.

Ordinary cursor commits and semantic cursor shapes keep the native `NSCursor`
path. A custom cursor commit that requests presentation feedback uses the shared
Metal auxiliary-surface presenter, which also displays guest drag icons. The
host tracks pointer position locally; moving its click-through panel neither
waits for the guest nor redraws an unchanged image. Native file-drag sessions
continue to use macOS file icons, independently of the guest icon surface.

`wp_presentation` v1 is advertised only after initial host clock calibration.
Window, queried cursor and drag feedback use actual drawable timestamps and the
same outcome journal. A zero result proves only that a particular host attempt
was not shown. If that Wayland commit remains current, its query waits for a
protected replay; it is discarded only after its content is superseded or
removed. Unroled commits also retain their legitimate pending queries. Auxiliary
surfaces whose pixels are not sampled cannot receive a parent's display result.
Visible current content automatically requests a fresh protected publication
after a skipped drawable, so static applications do not need to submit another
commit. The request waits for the guest's acknowledgement of the previous zero
result before rebinding its query; it never rereads a released source texture.
No hardware-clock, hardware-completion, zero-copy or constant-refresh claim is
made; v1 refresh prediction and presentation flags remain zero.

Host reconnect resets input authority and IME state, cancels unfinished drags and
replays retained scenes. Historical submitted display feedback is retained until
acknowledged or explicitly failed. Completed guest-only drops can continue
transferring data across a host disconnect.

Primary Selection and Linux middle-button paste are deliberately unsupported.
The ordinary clipboard remains separate from pointer-button delivery.

## Build and checks

`scripts/build-wayland.py` pins Wayland 1.25.0 and wayland-protocols 1.49 to exact
source commits. Libraries, scanner and XML are installed into a private prefix;
the compositor links the static server archive directly and hides its symbols
from dependency interposition. Builds validate restored cache contents and
regenerate/relink when the dependency stamp changes. Complete upstream notices
are included in release bundles.

Run on the matching Linux architecture/libc:

```sh
python3 scripts/build-wayland.py x86_64-gnu
make -C guest/compositor test-core-protocols test-input-protocol \
  test-activation test-xdg-output test-text-input test-presentation-time test-scene-admission test-flat-feedback \
  test-vmpipe-syncobj test-vmpipe-dmabuf test-file-drag test-scene-damage
```

The fixtures exercise real Wayland resources and emitted events while replacing
GPU/host collaborators. They do not establish GTK/Qt/Xwayland compatibility,
hardware synchronization or real display latency. Full guest builds and VM/SSH
application tests remain separate acceptance checks. The new host/guest wire
version is 15, so both ends must be rebuilt together.
