# Accessibility trees for agents

Agents read and operate Linux application windows through AT-SPI2, the
semantic UI tree that screen readers use. Both backends link
`accessibility.c` (Wayland thread) and `accessibility_worker.c` (one
ordinary-user GDBus thread). The worker talks D-Bus directly; it needs no
libatspi, Python or toolkit bindings.

## Why a worker thread

An application answers AT-SPI2 calls from its own main loop, and that loop may
itself be waiting on the compositor (a Wayland roundtrip, a frame callback).
Every D-Bus call therefore blocks only the worker. The Wayland thread copies
the target window's identity into each request: its client PID (none for
Xwayland), title, app ID and xdg window geometry origin. No Wayland object
crosses into the worker.

## Finding the window's tree

The worker asks the session bus's `org.a11y.Bus` for the accessibility bus
address and, once, sets `org.a11y.Status.IsEnabled`, which Qt, Chromium and
others need before they publish a tree (GTK always does). Without
`at-spi2-core` there is no accessibility bus; requests fail with an
explanation and agents use the screenshot fallback.

Registry children are applications; their children are top-level windows.
Applications whose bus connection belongs to the window's client PID are
searched first. Among their windows an exact title match, the `active` state and
the `showing` state score in that order. Without a PID match, only an exact
title identifies a window. Elements are addressed as the bus name immediately
followed by the object path (`:1.42/org/a11y/atspi/accessible/17`), and an
element must belong to the application that owns the addressed window.

## Operations

| Operation | Number | String | Effect |
| --- | --- | --- | --- |
| 1 tree | depth (0–64) | — | JSON tree below the window or `element` |
| 2 action | action index | action name | `Action.DoAction` |
| 3 focus | — | — | `Component.GrabFocus` |
| 4 set text | — | text (64 KiB) | `EditableText.SetTextContents`, else delete + insert |
| 5 set value | value | — | `Value.CurrentValue` |

Flags: 1 includes elements that are not `showing`; 2 reads up to 64K
characters of text instead of 1K. Text is read only for editable elements,
unnamed elements or on request, and never for `password text`.

Each node reports `ref`, `role` (`GetRoleName`), `name`, `description`,
`states`, `bounds`, `actions`, `text`, `value`/`minimum`/`maximum`, `children`
and, when children were left out, `childCount`. Bounds are top-left window
content points, the same space as host input: `Component.GetExtents` window
coordinates minus the xdg geometry origin, except for GTK 4, which already
measures from the window's content. A request stops at 2,000 nodes, 256
children per node, 1 MiB of JSON or 10 seconds and then sets `truncated`.

## Wire protocol

Window protocol version 16. Host NPW2 opcode 41 carries
`token:u32, window:u32, operation:u32, flags:u32, number:f64, element:string,
argument:string`. The reply is NPAP opcode 7 on the same lane as application
replies: `token:u32, error:string, json:string`; a nonempty error fails the
request. The worker queue holds 8 requests; overload, an unknown window and an
invalid request fail only that request. Replies from an earlier host session are
dropped.

## Checks

`make -C guest/compositor test-accessibility` runs the worker against a fake
registry and application on a private `dbus-run-session` bus: window
selection, hidden-element pruning, depth limits, actions by index and name,
focus, text and value changes, GTK 3 coordinate conversion, password redaction
and refusal of elements from another application. It does not replace a check
against real GTK, Qt or Electron applications in a VM.
