# NativePipe file RPC (NPFR v1)

A libc-only library shared by bootstrap, guestd, recovery and the compositor's
user file worker. It never changes UID/GID and never accepts a UID on the wire.
The caller chooses its existing process credentials and an open root directory.
Linux uses `openat2(RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS)` so recovery resolves
absolute symlinks against the mounted target system, not the initramfs.
When Rosetta returns `ENOSYS`, only a root descriptor identifying the process's
actual `/` may fall back to component-by-component `openat` resolution. Each
component is pinned with `O_PATH | O_NOFOLLOW`; ordinary symlinks are bounded
to 40 resolutions, and procfs symlinks are rejected conservatively, including
magic links. Virtual recovery roots fail closed without `openat2`. No fallback
occurs for permission errors. `make test-linux` also injects `ENOSYS` to check
this path on native Linux; its `build/file-open-test` runs under Rosetta too.

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
| 6 | flags (u16; WRITE: REPLACE=1, NOFOLLOW=2; MKDIR: NOFOLLOW=2; other types: zero) |
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

Lazy filesystem access adds SNAPSHOT=10, RANGE=11 and DIRECTORY=12. These
operations reject `.`/`..` components and every symbolic link, including
ancestors. They use `openat2(RESOLVE_IN_ROOT | RESOLVE_NO_SYMLINKS)`; the Rosetta
fallback keeps the existing actual-process-root restriction and walks without
following links. Ordinary STAT/READ/WRITE retain their original semantics.

SNAPSHOT returns METADATA plus a 48-byte opaque revision: device:u64, inode:u64,
size:u64, mtime_seconds:i64, mtime_nanoseconds:u32, ctime_seconds:i64,
ctime_nanoseconds:u32. RANGE appends offset:u64, length:u32, and that revision
to its path request. It transfers at most the requested length using `pread`,
then END with the actual byte count. Individual requests are limited to 1 MiB;
there is no aggregate file-size limit. EOF returns a short or empty range.
Offsets and offset+length must fit signed 64-bit file offsets. A revision
mismatch before or after reading returns status 116; the client discards all
bytes from that request. DIRECTORY pins a no-link directory and returns the
same METADATA/ENTRIES/END listing format as LIST.

Directory browsing uses BROWSE=13. Its path may be empty to open the worker's
actual home (`HOME`, then the effective user's passwd entry); no host username
or root-shell command is involved. It rejects symbolic links throughout the
requested path. The first METADATA carries path_length:u32, home_length:u32,
path bytes, home bytes. Each ENTRIES batch contains name_length:u16 followed by
the 28-byte stat metadata and name bytes. Attributes come from
`fstatat(AT_SYMLINK_NOFOLLOW)`, so links and special files can be displayed
without opening them. A name and its metadata never cross a frame boundary.
END carries the total entry count:u64. This is one connection per directory,
without an additional stat request per entry. The server streams 64 KiB
batches; the Swift client bounds accumulated listing metadata at 64 MiB, which
is separate from file content and does not limit transferred file sizes.

Exact-destination uploads use CREATE_STAGING=14, PUBLISH_STAGING=15 and
DISCARD_STAGING=16. All three obey the service's read-only policy and reject
symbolic links in their paths. CREATE_STAGING appends a random 32-byte ownership
token chosen by the client before sending the request. It exclusively creates a
private 0700 sibling directory named `.nativepipe-upload-UUID` and a 0600
`.nativepipe-owner` marker that binds the token to the directory's device and
inode. Its METADATA response echoes the token. Knowing the token before CREATE
allows cleanup even if cancellation loses that acknowledgement.

The existing recursive upload writes a file or directory tree into `.payload`
inside this directory, using WRITE/MKDIR with NOFOLLOW. PUBLISH_STAGING appends
the token, destination_length:u32 and full destination path. The target must
be in the same pinned parent directory. A native exclusive rename publishes
`.payload` atomically (`renameat2(RENAME_NOREPLACE)` on Linux,
`renameatx_np(RENAME_EXCL)` on macOS); unavailable native support fails closed.
An existing file, folder or symbolic link is never overwritten or merged.
Cancellation is checked before commit; the short commit finishes independently
so the reported result reflects publication. DISCARD_STAGING appends the token
and removes only the verified owned tree, without following any nested link.
Both return an empty END. Arbitrary paths, markerless directories and copied
markers cannot authorize cleanup. On transfer failure/cancellation, the client
uses a fresh cancellation-independent operation for cleanup. An unreachable
service can leave an unpublished private staging directory; no unrelated file
is removed to compensate. Progress becomes complete only after publication.

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
