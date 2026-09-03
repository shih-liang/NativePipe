# xwayland-satellite runtime input

RemotePipe builds `xwayland-satellite` from the exact upstream commit in
`UPSTREAM_COMMIT`, applies the checked-in protocol-state patch, and uses the
upstream locked Rust dependency graph. Run the build in the matching musl or
glibc environment:

```
make ARCH=aarch64 LIBC=gnu dist-target
```

One invocation builds only its native architecture/libc target. Release CI
uses four isolated build jobs. Artifact checksums belong to the signed release
manifest; a checked-in checksum of a locally built binary would not establish
its provenance. The guest
must also provide Xwayland 23.1 or newer plus the runtime libraries required by
the selected binary (`libxcb`, its Composite/RandR/Res extensions, and
`xcb-util-cursor`). `guestd` installs the selected binary at
`/usr/libexec/nativepipe/xwayland-satellite`; the compositor starts it lazily
when the first X11 client connects.

`wl_pointer.leave` must only be forwarded to Xwayland after the corresponding
enter was forwarded. Upstream v0.8.2 delays popup enters until the first motion
but used to forward a leave that arrived before that motion; Xwayland then
underflowed `pointer_enter_count`, leaving GTK hover/click state inconsistent.
The patch above fixes that protocol-state error at its source.

NativePipe requests `-glamor gl`, so Xwayland exposes DRI3 and accelerates its
rootless compositing through the distribution's Mesa VirGL driver and host
ANGLE/Metal renderer. Vulkan X11 clients independently use Venus. Do not
replace it with `-glamor none`: that selects Xwayland's `-shm` backend and
makes Vulkan X11 WSI unavailable.
