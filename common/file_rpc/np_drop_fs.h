#ifndef NP_DROP_FS_H
#define NP_DROP_FS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/file.h>
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
#define NP_DROP_MAX_WRITE (128u * 1024u)

/* Requests the guest sends (NPFR v2 opcodes). Tokens are 36 ASCII bytes;
 * "path" below is token + a relative path inside the drop ("" is the token's
 * directory, "name" the dropped item, "name/sub/file" inside a dropped folder). */
enum np_drop_opcode {
    NP_DROP_LOOKUP = 1,   /* path -> attr */
    NP_DROP_LIST = 2,     /* start:u32, then nothing (all tokens) or path -> DATA entries from index
                           * `start`: name_length:u16, name, attr; fewer than all when large */
    NP_DROP_OPEN = 3,     /* flags:u32 (Linux open flags), path -> handle:u64, attr */
    NP_DROP_READ = 4,     /* handle:u64, offset:u64, length:u32 (at most NP_DROP_READ_MAX) -> DATA */
    NP_DROP_RELEASE = 5,  /* handle:u64 -> empty */
    NP_DROP_WRITE = 6,    /* handle:u64, offset:u64; DATA bytes (at most NP_DROP_READ_MAX) -> written:u32 */
    NP_DROP_SETATTR = 7,  /* NP_DROP_SETATTR_SIZE bytes, path -> attr */
    NP_DROP_CREATE = 8,   /* flags:u32, mode:u32, path -> handle:u64, attr */
    NP_DROP_UNLINK = 9,   /* path -> empty */
    NP_DROP_RENAME = 10,  /* flags:u32, old_length:u16, token, old name, new name -> empty */
    NP_DROP_FSYNC = 11,   /* handle:u64 -> empty */
    NP_DROP_FLUSH = 12,   /* handle:u64, owner:u64 -> empty; releases the owner's POSIX locks */
    NP_DROP_GETLK = 13,   /* lock request -> start:u64, end:u64, type:u32, pid:u32 */
    NP_DROP_SETLK = 14,   /* lock request -> empty */
    NP_DROP_RMDIR = 15,   /* path -> empty */
    NP_DROP_MKDIR = 16,   /* mode:u32, path -> attr */
    NP_DROP_SYMLINK = 17, /* target_length:u16, target, path -> attr */
    NP_DROP_READLINK = 18, /* path -> DATA with the target */
};
/* SETATTR: valid:u32 (FUSE FATTR_* bits), mode:u32, size:u64, atime_seconds:i64,
 * atime_nanoseconds:u32, mtime_seconds:i64, mtime_nanoseconds:u32, handle:u64 */
#define NP_DROP_SETATTR_SIZE 48u
enum {
    NP_DROP_SET_MODE = 1, NP_DROP_SET_UID = 2, NP_DROP_SET_GID = 4, NP_DROP_SET_SIZE = 8,
    NP_DROP_SET_ATIME = 16, NP_DROP_SET_MTIME = 32, NP_DROP_SET_HANDLE = 64,
    NP_DROP_SET_ATIME_NOW = 128, NP_DROP_SET_MTIME_NOW = 256,
};
/* Lock request: handle:u64, owner:u64, start:u64, end:u64 (inclusive; INT64_MAX
 * means to the end), type:u32 (Linux F_RDLCK 0, F_WRLCK 1, F_UNLCK 2), flags:u32 */
#define NP_DROP_LOCK_SIZE 40u
enum { NP_DROP_LOCK_FLOCK = 1, NP_DROP_LOCK_WAIT = 2 };
enum { NP_DROP_RENAME_NOREPLACE = 1, NP_DROP_RENAME_EXCHANGE = 2 };
/* Notifications the guest sends. */
enum np_drop_notification {
    /* The guest's mount path for this session's drops. */
    NP_DROP_MOUNTED = 1,
};
#define NP_DROP_TOKEN 36u
#define NP_DROP_READ_MAX (1024u * 1024u)
#define NP_DROP_NAME_MAX 255u
#define NP_DROP_PATH_MAX 4095u

/* kind:u8 (1 directory, 2 file, 3 symbolic link), reserved:3, mode:u32 (permission bits),
 * size:u64, mtime_seconds:i64, mtime_nanoseconds:u32, reserved:u32, file_id:u64 */
#define NP_DROP_ATTR 40u
enum np_drop_kind { NP_DROP_DIRECTORY = 1, NP_DROP_FILE = 2, NP_DROP_LINK = 3 };
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
    NP_DROP_ESTALE = 116, NP_DROP_EINTR = 4, NP_DROP_EAGAIN = 11, NP_DROP_EEXIST = 17,
    NP_DROP_EXDEV = 18, NP_DROP_ENOSPC = 28, NP_DROP_EDEADLK = 35, NP_DROP_ENOLCK = 37,
    NP_DROP_ENOTEMPTY = 39, NP_DROP_EOPNOTSUPP = 95, NP_DROP_EDQUOT = 122, NP_DROP_EBUSY = 16,
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
 * and INTERRUPT). Returns -1 only for a malformed request. Thread-safe; a
 * blocking lock request waits on its own thread until granted or interrupted,
 * so the caller should serve requests from several threads. */
int np_drop_fs_handle(struct np_drop_fs *fs, const void *request, size_t length,
                      np_drop_reply reply, void *context);
/* After DESTROY the kernel expects no further replies. */
int np_drop_fs_destroyed(struct np_drop_fs *fs);

/* For the Swift host: flock(2) is hidden there by the struct of the same name. */
static inline int np_drop_flock(int descriptor, int operation) { return flock(descriptor, operation); }

#ifdef __linux__
/* Mounts through guestd, listens for the host on NP_FILE2_PORT and serves
 * /dev/fuse on worker threads. Runs as the session's ordinary user. */
struct np_drop_service;
struct np_drop_service *np_drop_service_start(void);
void np_drop_service_stop(struct np_drop_service *service);
#endif

#endif
