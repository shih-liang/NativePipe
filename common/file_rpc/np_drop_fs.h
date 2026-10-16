#ifndef NP_DROP_FS_H
#define NP_DROP_FS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "np_file_session.h"

/* Drag-and-drop files as a guest FUSE filesystem, served by the host over an
 * NPFR v2 session. The guest sees /run/nativepipe/drops/<uid>/<token>/<name>;
 * the host maps each token to the dropped file's path and opens it like any
 * Mac app. See README.md "Drop files". */

/* guestd mounts for the requesting user and passes the /dev/fuse descriptor
 * back with SCM_RIGHTS. Request: "NPDM". Reply: "NPDM", status:u32 (errno),
 * path_length:u16, path bytes; the descriptor accompanies a zero status. */
#define NP_DROP_MOUNT_SOCKET "/run/nativepipe/drops.sock"
#define NP_DROP_MOUNT_ROOT "/run/nativepipe/drops"
#define NP_DROP_FS_NAME "nativepipe-drops"
#define NP_DROP_MAX_READ (128u * 1024u)

/* Requests the guest sends (NPFR v2 opcodes). Tokens are 36 ASCII bytes. */
enum np_drop_opcode {
    /* token + name (empty: the token's directory) -> attr */
    NP_DROP_LOOKUP = 1,
    /* empty (all tokens) or token -> DATA entries: name_length:u16, name, attr */
    NP_DROP_LIST = 2,
    /* token + name -> handle:u64, attr */
    NP_DROP_OPEN = 3,
    /* handle:u64, offset:u64, length:u32 (at most NP_DROP_READ_MAX) -> DATA */
    NP_DROP_READ = 4,
    /* handle:u64 -> empty */
    NP_DROP_RELEASE = 5,
};
/* Notifications the guest sends. */
enum np_drop_notification {
    /* The guest's mount path for this session's drops. */
    NP_DROP_MOUNTED = 1,
};
#define NP_DROP_TOKEN 36u
#define NP_DROP_READ_MAX (1024u * 1024u)
#define NP_DROP_NAME_MAX 255u

/* kind:u8 (1 directory, 2 file), reserved:3, mode:u32 (permission bits),
 * size:u64, mtime_seconds:i64, mtime_nanoseconds:u32, reserved:u32, file_id:u64 */
#define NP_DROP_ATTR 40u
enum np_drop_kind { NP_DROP_DIRECTORY = 1, NP_DROP_FILE = 2 };
struct np_drop_attr {
    uint8_t kind;
    uint32_t mode;
    uint64_t size;
    int64_t mtime_seconds;
    uint32_t mtime_nanoseconds;
    uint64_t file_id;
};
void np_drop_attr_encode(unsigned char out[NP_DROP_ATTR], const struct np_drop_attr *attr);
int np_drop_attr_decode(const unsigned char *in, size_t length, struct np_drop_attr *attr);

/* Response statuses are Linux errno values on both sides of the wire. */
enum np_drop_status {
    NP_DROP_OK = 0, NP_DROP_EPERM = 1, NP_DROP_ENOENT = 2, NP_DROP_EIO = 5,
    NP_DROP_EBADF = 9, NP_DROP_EACCES = 13, NP_DROP_ENOTDIR = 20, NP_DROP_EISDIR = 21,
    NP_DROP_EINVAL = 22, NP_DROP_EROFS = 30, NP_DROP_ENAMETOOLONG = 36, NP_DROP_ELOOP = 40,
    NP_DROP_ESTALE = 116,
};

/* The FUSE request handler. It never touches the guest filesystem: every
 * answer comes from the host session, which may be absent or replaced. */
struct np_drop_fs;
typedef int (*np_drop_reply)(void *context, const struct iovec *vector, int count);
struct np_drop_fs *np_drop_fs_create(uid_t owner, gid_t group);
void np_drop_fs_destroy(struct np_drop_fs *fs);
/* Not owned. Returns once no request still uses the previous session, so the
 * caller may close it afterwards. */
void np_drop_fs_set_session(struct np_drop_fs *fs, struct np_file2_session *session);
/* Handles one /dev/fuse request; replies through `reply` (nothing for FORGET
 * and INTERRUPT). Returns -1 only for a malformed request. Thread-safe. */
int np_drop_fs_handle(struct np_drop_fs *fs, const void *request, size_t length,
                      np_drop_reply reply, void *context);
/* After DESTROY the kernel expects no further replies. */
int np_drop_fs_destroyed(struct np_drop_fs *fs);

#ifdef __linux__
/* Mounts through guestd, listens for the host on NP_FILE2_PORT and serves
 * /dev/fuse on worker threads. Runs as the session's ordinary user. */
struct np_drop_service;
struct np_drop_service *np_drop_service_start(void);
void np_drop_service_stop(struct np_drop_service *service);
#endif

#endif
