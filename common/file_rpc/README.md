# NativePipe file RPC (NPFR v1)

A libc-only library shared by bootstrap, guestd, recovery and the compositor's
user file worker. It never changes UID/GID and never accepts a UID on the wire.
The caller chooses its existing process credentials and an open root directory.
Linux uses `openat2(RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS)` so recovery resolves
absolute symlinks against the mounted target system, not the initramfs.

## Transport

One operation per vsock connection. Root services listen on 1025; the graphical
user service listens on 1026. Both reject non-host CIDs, including guest-local
vsock loopback. Neither port is a display/input/control channel.

All integers are little-endian. The 16-byte frame header is:

| Offset | Field |
| --- | --- |
| 0 | `NPFR` (4 bytes) |
| 4 | version = 1 (u8) |
| 5 | type (u8) |
| 6 | flags (u16; WRITE may set REPLACE=1, otherwise zero) |
| 8 | payload size (u32, at most 65536) |
| 12 | errno status (u32, zero for success) |

Requests STAT=1, LIST=2, READ=3, WRITE=4, MKDIR=9 contain `path_length:u32`, path bytes
(absolute, no NUL, at most 4095), and WRITE additionally `mode:u32`.
WRITE without REPLACE fails if the destination exists. No shell quoting is used.
MKDIR creates one directory with mode 0700, failing if it already exists.
Clients stream directory trees as individual entries and file streams, not tar
archives or paths interpreted by a shell.

METADATA=8 contains mode:u32, uid:u32, gid:u32, size:u64, mtime:i64.
STAT ends at METADATA. READ also accepts directories and selects the appropriate
stream after METADATA. Regular files are read to EOF, including size-zero procfs
files; `st_size` is a progress hint, not a transfer bound.

For reads and directory listings, DATA=5 contains at most 64 KiB. ENTRIES=6 contains a batch of
`type:u8, name_length:u16, name_bytes` records; names are never split. END=7
contains the transferred byte count (u64) for files, and no body for directories.
A nonzero END status is failure, never success or implicit EOF.

For writes, the server replies with METADATA and becomes the receiver. After
the sender's END, the server checks chmod, fsync and close, then acknowledges
with its own END. The sender must receive this final acknowledgement.

The cancelling caller shuts down/closes its
connection, waking blocked socket I/O. No cancellation affects another file.
SOCK_STREAM provides backpressure without per-chunk acknowledgements; no whole-file buffers, unlimited
queues or global send locks are needed. The listener caps concurrent operations
at 16 and joins all workers before recovery unmounts or switches root.

The same DATA/END functions frame bootstrap artifacts (NPAG request flag 1).
Flag 0 still serves exact-length bytes through the same library so an installed
bootstrap can fetch its first update; named-artifact negotiation stays NPAG v1.
Artifacts retain their existing named-file/version negotiation, not a second
file transport implementation. `make test` uses socketpairs and temporary
files; Linux additionally checks root resolution and special-file rejection.

WRITE updates an explicitly requested destination directly. On interruption it
reports failure; a partial destination may remain. It never deletes a path it
did not create. Bootstrap installation retains its existing adjacent staging
and rename so a failed update cannot replace the running executable.
