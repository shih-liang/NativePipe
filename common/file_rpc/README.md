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

## NPFR v2 sessions

`np_file_session.c` (C, guest) and `FileSession.swift` (host) implement one
long-lived, full-duplex connection carrying many concurrent streams. It is a
transport only; operations are defined by the users of its opcodes. v1 above is
unchanged and keeps serving recovery, bootstrap and transfers.

The guest keeps listening (vsock 1031, host CID only); the host connects. After
that either side opens streams and sends notifications. Host-opened stream IDs
are even, guest-opened IDs odd; 0 is the connection. IDs only increase.

The 16-byte header is `NP2F`, type:u8, flags:u8, reserved:u16 (zero),
stream_id:u32, length:u32 (at most 65536), all little-endian.

| Type | Stream | Payload |
| --- | --- | --- |
| HELLO=1 | 0 | version:u8 = 2, role:u8 (host 0, guest 1), reserved:u16, capabilities:u32. First frame from each side; the roles must differ. |
| REQUEST=2 | new | opcode:u16, reserved:u16, body. Opens the stream. |
| RESPONSE=3 | existing | status:u32, body. Exactly once from the responder, before its DATA. |
| DATA=4 | existing | bytes. Counts against credit. Empty DATA only carries END. |
| NOTIFY=5 | 0 | opcode:u16, reserved:u16, body. |
| CANCEL=6 | existing | empty. Abandons both directions; queued data is dropped. |
| CREDIT=7 | 0 or existing | increment:u32 (nonzero). |

Only REQUEST, RESPONSE and DATA may set END_STREAM (flag 1), which finishes
the sender's half. A stream is complete when both halves ended or either side
cancelled; dropping the last local reference to an unfinished stream cancels it.

Flow control: each direction of a stream starts with 256 KiB of credit and the
connection with 4 MiB; DATA consumes both. A receiver returns credit as the
application consumes data, so memory per stream and per connection is bounded.
A stalled reader holds at most its stream's window, so other streams keep
moving until stalled streams fill the connection window (16 full streams). Exceeding credit,
out-of-order or wrong-parity IDs, a second RESPONSE, DATA after END or before
RESPONSE, and unknown types or flags end the session with EPROTO. Frames for a
stream that is already gone are dropped, with connection credit returned.

Frames are written whole. Control frames (everything except DATA) overtake
queued DATA. The C reader thread never writes, so two peers with full socket
buffers cannot deadlock; frames it originates go through a writer thread.
Cancelling or releasing a stream only queues its CANCEL, so it never blocks.
Request and notification callbacks run on the reader and must not block or
close the session. A peer accepts at most 256 concurrent inbound streams and
cancels the rest; a peer that lets 65536 queued control frames (credit and
refusals) accumulate without reading is dropped with ENOBUFS.

`make test` runs `tests_session.c` over socketpairs; the Swift
`FileSessionTests` also exercise Swift↔C in both roles.

## Drop files

Mac files dropped on a Linux window are served to the guest as a FUSE
filesystem instead of being copied. `np_drop_fs.c` is the guest's FUSE request
handler; `np_drop_service.c` (Linux only, linked into the compositor) runs it;
`DropFileServer.swift` answers on the host. Both sides use NPFR v2 on port 1031.

**Mount.** The compositor runs as the session's ordinary user, so it asks guestd
over `/run/nativepipe/drops.sock` (`NPDM`). guestd reads the caller's UID/GID
with `SO_PEERCRED`, mounts `fuse` at `/run/nativepipe/drops/<uid>` (root-owned
directories, `nosuid,nodev`, `user_id`/`group_id` of the caller,
`default_permissions`, no `allow_other`) and passes the `/dev/fuse` descriptor
back with `SCM_RIGHTS`, followed by the mount path. The newest request replaces
an older mount for that user, so a restarted compositor takes over. Root is
refused. No descriptor or mount point is ever placed in a user-writable path.

**Names.** The guest sees `<mount>/<token>/<name>`: one directory per drop,
named by a random UUID, containing only that file. The host maps the token to
the dropped path and the name must match it exactly.

**Requests** (guest to host; statuses are Linux errno values, mapped on the host).
"path" is token (36 bytes) + name, where an empty name means the token's directory:

| Opcode | Body | Response |
| --- | --- | --- |
| LOOKUP=1 | path | attributes |
| LIST=2 | empty (all tokens) or token | DATA: name_length:u16, name, attributes, repeated |
| OPEN=3 | flags:u32 (Linux open flags), path | handle:u64, attributes |
| READ=4 | handle:u64, offset:u64, length:u32 (at most 1 MiB) | DATA with the bytes; short at EOF |
| RELEASE=5 | handle:u64 | empty |
| WRITE=6 | handle:u64, offset:u64; then DATA with the bytes (at most 1 MiB) | written:u32 |
| SETATTR=7 | valid:u32 (FUSE FATTR_* bits), mode:u32, size:u64, atime:i64+u32, mtime:i64+u32, handle:u64, path | attributes |
| CREATE=8 | flags:u32, mode:u32, path | handle:u64, attributes |
| UNLINK=9 | path | empty |
| RENAME=10 | flags:u32 (NOREPLACE 1), old_length:u16, token, old name, new name | empty |
| FSYNC=11 | handle:u64 | empty |
| FLUSH=12 | handle:u64, owner:u64 | empty; ends the owner's POSIX locks on the file |
| GETLK=13 | handle, owner, start, end (u64 each; inclusive, INT64_MAX = to the end), type:u32 (F_RDLCK 0, F_WRLCK 1, F_UNLCK 2), flags:u32 | start:u64, end:u64, type:u32, pid:u32 |
| SETLK=14 | as GETLK; flags: FLOCK 1 (a flock(2) lock), WAIT 2 (block until granted) | empty |

Attributes are 40 bytes: kind:u8 (1 directory, 2 file), reserved:3, mode:u32
(permission bits), size:u64, mtime_seconds:i64, mtime_nanoseconds:u32,
reserved:u32, file_id:u64. SETATTR with only the handle bit is fstat of that
open file. The guest sends NOTIFY MOUNTED=1 with its mount path when the host
connects; the host builds guest URLs from it.

**Semantics.** The host behaves like any second Mac app with the file open: the
identity is the path. LOOKUP and OPEN look at the path each time; a handle keeps
the file it opened, so after an atomic save it still reads the old contents
while a new open gets the new file. FUSE entry and attribute timeouts are zero
and opens do not keep the page cache, so the guest sees host changes on its next
stat or open. Handles belong to their session and close with it; drops outlive
sessions so a compositor restart keeps them.

Writes follow POSIX, not any application's save strategy:

- Writing through a handle writes the Mac file. O_TRUNC and SETATTR sizes truncate it.
- Other names in a drop's directory (temporary files, backups) are files the
  guest created; the host keeps them in a private staging directory, never in
  the Mac folder, and deletes them with the drop.
- RENAME of such a file onto the dropped name replaces the Mac file, as a Mac
  app's safe save does (`replaceItemAt`, or an in-place copy when the item
  cannot be replaced; a Mac file deleted meanwhile is recreated).
- RENAME of the dropped name to another name gives that name a copy and hides
  the dropped name; the Mac file stays where it is. Creating the dropped name
  again then writes the Mac file from empty (vim's rename-and-write save).
- UNLINK of the dropped name only hides it from the guest; the Mac file stays.
- Renames stay within one drop's directory (EXDEV otherwise); RENAME_EXCHANGE,
  directories, links and special files are refused.

Locks are real on the Mac side, so Mac and Linux programs that lock exclude each
other. POSIX locks become open-file-description locks on a descriptor per guest
lock owner and file, giving POSIX ownership (any number of opens, one owner);
FLUSH, which the kernel sends on every close(2), releases them. flock(2) locks
are taken on the handle's own descriptor and end with it. A waiting SETLK polls
until granted; FUSE INTERRUPT cancels the stream and the guest answers EINTR.
Blocking lock requests run on their own threads in the guest service.

`make test` runs `tests_drop.c` (the FUSE handler against a mock host); the
Swift `DropFileServerTests` cover the host (writes, safe saves, backups, locks
against Mac programs) and drive the C handler end to end.
