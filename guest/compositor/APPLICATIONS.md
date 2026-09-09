# Applications in a NativePipe session

Both backends link `applications.c`, `applications_worker.c` and
`application_icons.c`. The compositor owns one ordinary-user worker with a
private GLib main context. All GIO catalog queries, icon I/O/decoding and
desktop-entry launches execute there. Wayland's event thread only queues a
request or sends a completed response. It never runs a catalog scan or shell.

The worker uses the compositor's real uid, HOME, XDG environment and locale.
GIO owns desktop ID derivation, XDG precedence (including Hidden overrides),
OnlyShowIn/NotShowIn, TryExec, localized metadata, Path and Exec field codes.
Launch requests contain an ID, not a cached executable/argument vector.
Guestd remains the root administration/CLI service; application browsing and
desktop-ID launching do not call guestd or root file RPC.

VM sessions already run under dbus-run-session. The remote backend also starts
a private bus before opening its own display. Its stdin/stdout are separated
from the SSH protocol streams, so activated services cannot corrupt the wire
with log output. The worker publishes the actual
Wayland/Xwayland environment to that bus before activating desktop entries.
The original entry (and its filename for `%k`) is preserved. A successful D-Bus
activation returns PID 0, meaning no child PID is available, not a fake process.
Spawned applications report their real PID and exit status. An unavailable or
unconfigured private activation service is an error, never a fallback to the
user's physical desktop bus.

## Wire protocol

Window protocol version 9 requires a matching host and compositor. The
existing NPIP framing and host control/event lanes are retained; there is no
new listener, JSON object or per-application process in the host.

Host NPW2 opcode 27 carries `token:u32, action:u32, desktopID:string`.
Actions are list (0, empty ID), launch (1), thumbnail (2), and appearance
(3, `dark` or `light` in the string field). Appearance applies the same GNOME
color-scheme and GTK-theme settings used by VMHost's desktop integration.
NPAP uses an 8-byte header (`NPAP`, direction=1, opcode, two zero bytes), then:

| Opcode | Body |
| --- | --- |
| 1 | token, count (0–32), metadata records |
| 2 | token, pid:i32, error:string; empty error means launch succeeded |
| 3 | token=0; application database changed |
| 4 | token, error:string; completes a catalog, or fails any request if error is nonempty |
| 5 | token, PNG:bytes; empty bytes means no usable icon |
| 6 | token=0, pid:i32, exitStatus:i32 |

Metadata fields: ID, display name, comment, executable (identity matching only),
StartupWMClass and icon name, all length-prefixed UTF-8 strings. Integers are
little-endian. No 512-application truncation remains. Transport trust limits
produce explicit errors instead of partial catalogs.
Metadata batches are also limited to 64 KiB. Oversized or invalid entries fail
that catalog request, without disconnecting the display channel.

Images are fetched separately with four outstanding host requests, decoded as
the session user and converted to 64-pixel PNGs. Icon lookup uses the user's
GTK/GSettings theme, XDG icon roots, `index.theme` sizes and inheritance,
hicolor and unthemed/pixmap fallbacks. SVG is decoded through linked librsvg
(not a build-machine GdkPixbuf module-cache path); PNG/XPM use GdkPixbuf.

## Invalidation and lifetime

GAppInfoMonitor marks the host catalog dirty; it does not rescan on every
filesystem notification. The shared Swift ApplicationClient coalesces active
loads, caches empty catalogs too, and rejects a snapshot invalidated during
loading. VM manager notifications and visible launcher snapshot revisions
invalidate presentation caches. Unchanged revisions do not retransmit icons.
Mapping another window no longer forces a rescan. Metadata returns before icon
requests finish. File monitors and icon-theme changes invalidate the same host
cache; there is no second thumbnail byte cache in the compositor.

## Shared host integration and file drag

VMHost's macOS appearance observation, keyboard-source observation and preference
resolution live in `NativePipeWindowing.HostIntegrationController`. VMHost,
RemoteHost and the standalone CLI use that implementation. Both FluxWindow
hosts also use one settings-to-window-preferences mapping. Backend code only
delivers resolved values through VM control or the SSH application worker.

Both hosts use `WindowBridge`, `ClipboardBridge`, `FileDragBridge` and the same
AppKit file-promise provider. NPW2 host opcode 28 / guest opcode 37 carry only
drag tokens, coordinates, copy negotiation and file URI lists (at most 1 MiB),
not file contents. Dropped transfers outlive subsequent drag gestures. A guest
source receives `dnd_finished` after its promised copies complete; cancellation
and disconnect do not release a new connection's source.

VM imports clone regular files into transient read-only shares; if cloning is
unavailable, ordinary-user NPFR on vsock 1026 copies directly into Linux. Selected
directories can be shared directly. Temporary promised directories are copied,
not shared by reference. Guest exports copy into the user's chosen destination,
never a shared directory. Remote uses SFTP for both directions. Clipboard file
URLs use these same transfer services; Linux URLs are never published as local
Mac file URLs.

Worker command and response queues are bounded (32 and 4). Overloaded requests
report a busy error without closing the window connection. Responses from an
old transport generation cannot enter a reconnected session. Disconnect skips
queued obsolete commands, and shutdown wakes blocked producers before joining
the worker. These controls do not gate rendering or resize configures.

## Build and checks

Runtime/build dependencies: GIO, GdkPixbuf and librsvg (plus existing compositor
dependencies). VM install adapters include these distro packages. Remote release
packaging includes image dependencies and matching PNG/XPM loader modules,
but no FFmpeg, VA-API or codec-only dependencies. System FFmpeg libraries must
match the binary's required major versions. GLib/GIO and their dconf/GVfs plugin
runtime remain the distribution's responsibility, like `dbus-run-session` and
EGL/GBM. Private libraries resolve
with ELF RUNPATH, not an LD_LIBRARY_PATH inherited by desktop applications.

`make -C guest/compositor test-applications` runs Linux integration fixtures as
an unprivileged user: 600+ applications, XDG overrides, nested IDs, filtering,
database notifications, inherited SVG/XPM icons, FIFO rejection, exact launch
arguments/working directory, D-Bus-only activation and errors, process exit and
shutdown under backpressure.
It is included in Linux CI and all four release build variants. Swift
ApplicationClientTests check shared VM/remote framing, coalescing, invalidation,
disconnect and error handling. Neither test substitutes for an installed
VM/SSH application launch check.

`make -C guest/compositor test-file-drag` exercises real Wayland resource lifetime
and pipe EOF for negotiation, overlapping drops, finish/cancel, export and
disconnect. It does not substitute for Finder/GTK interactive drag testing.
