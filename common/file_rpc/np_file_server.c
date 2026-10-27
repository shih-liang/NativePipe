#define _GNU_SOURCE
#include "np_file_rpc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/magic.h>
#include <linux/openat2.h>
#include <linux/vm_sockets.h>
#include <sys/vfs.h>
#include <sys/syscall.h>
#endif

int np_file_peer_is_host(int socket) {
#ifdef __linux__
    struct sockaddr_vm peer = {0}; socklen_t size = sizeof(peer);
    return getpeername(socket, (struct sockaddr *)&peer, &size) == 0 &&
        size == sizeof(peer) && peer.svm_family == AF_VSOCK && peer.svm_cid == VMADDR_CID_HOST;
#else
    (void)socket; return 0;
#endif
}
int np_file_listen_vsock(uint32_t port) {
#ifdef __linux__
    int fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_vm address = {.svm_family = AF_VSOCK, .svm_cid = VMADDR_CID_ANY, .svm_port = port};
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(fd, 16) == 0) return fd;
    int error = errno; close(fd); errno = error; return -1;
#else
    (void)port; errno = ENOTSUP; return -1;
#endif
}
#ifdef __linux__
/* Rosetta does not implement openat2. Only the process's real root can use
 * this fallback: an openat walk cannot reproduce RESOLVE_IN_ROOT for a
 * recovery directory that another process might rename during resolution.
 * Pin every traversed inode, inspect symlinks without following them, and
 * refuse procfs links (a conservative superset of Linux magic links). */
static int open_process_root(int root_fd, const char *path, int flags, unsigned mode) {
    struct stat supplied, actual;
    if (fstat(root_fd, &supplied) < 0 || stat("/", &actual) < 0) return -1;
    if (supplied.st_dev != actual.st_dev || supplied.st_ino != actual.st_ino) {
        errno = ENOSYS; return -1;
    }
    int directory = fcntl(root_fd, F_DUPFD_CLOEXEC, 0), result = -1;
    if (directory < 0) return -1;
    char pending[4096]; strcpy(pending, path);
    unsigned links = 0;
    for (;;) {
        char *start = pending;
        while (*start == '/') start++;
        if (!*start) start = ".";
        size_t length = strcspn(start, "/");
        char part[4096]; memcpy(part, start, length); part[length] = 0;
        const char *tail = start + length;
        int more = *tail != 0;
        while (*tail == '/') tail++;
        /* A trailing slash requires a directory, including on a symlink. */
        if (more && !*tail) tail = ".";
        int pinned = openat(directory, part, O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (pinned < 0) {
            if (!more && errno == ENOENT && (flags & O_CREAT))
                result = openat(directory, part, flags | O_NOFOLLOW | O_CLOEXEC, mode);
            break;
        }
        struct stat st;
        if (fstat(pinned, &st) < 0) {
            int error = errno; close(pinned); errno = error; break;
        }
        if (S_ISLNK(st.st_mode)) {
            if (!more && (flags & O_NOFOLLOW)) {
                if ((flags & O_PATH) && !(flags & O_DIRECTORY)) { result = pinned; break; }
                close(pinned); errno = (flags & O_DIRECTORY) ? ENOTDIR : ELOOP; break;
            }
            if (!more && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
                close(pinned); errno = EEXIST; break;
            }
            struct statfs fs;
            if (fstatfs(pinned, &fs) < 0) {
                int error = errno; close(pinned); errno = error; break;
            }
            if (fs.f_type == PROC_SUPER_MAGIC || ++links > 40) {
                close(pinned); errno = ELOOP; break;
            }
            char target[4096];
            ssize_t n = readlinkat(pinned, "", target, sizeof(target));
            int error = errno; close(pinned); errno = error;
            if (n < 0) break;
            size_t suffix = more ? strlen(tail) + 1 : 0;
            if (!n || (size_t)n + suffix >= sizeof(target)) { errno = ENAMETOOLONG; break; }
            target[n] = 0;
            if (more) { target[n] = '/'; strcpy(target + n + 1, tail); }
            if (target[0] == '/') {
                int next = fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
                if (next < 0) break;
                close(directory); directory = next;
            }
            strcpy(pending, target);
        } else if (more) {
            if (!S_ISDIR(st.st_mode)) { close(pinned); errno = ENOTDIR; break; }
            close(directory); directory = pinned;
            memmove(pending, tail, strlen(tail) + 1);
        } else {
            close(pinned);
            /* O_NOFOLLOW also rejects a link substituted since inspection. */
            result = openat(directory, part, flags | O_NOFOLLOW | O_CLOEXEC, mode);
            break;
        }
    }
    int error = errno; close(directory); errno = error; return result;
}
#endif
int np_file_open(int root_fd, const char *path, int flags, unsigned mode) {
    if (!path || path[0] != '/' || strlen(path) > 4095) { errno = EINVAL; return -1; }
#ifdef __linux__
    struct open_how how = {.flags = (uint64_t)(flags | O_CLOEXEC), .mode = mode,
                          .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS};
#ifdef NP_FILE_TEST_OPENAT2_ENOSYS
    (void)how;
    int fd = -1; errno = ENOSYS;
#else
    int fd = (int)syscall(SYS_openat2, root_fd, path, &how, sizeof(how));
#endif
    if (fd >= 0 || errno != ENOSYS) return fd;
    return open_process_root(root_fd, path, flags, mode);
#else
    /* Host tests do not resolve symlinks. Walk each component, so neither an
     * intermediate symlink nor '..' can escape the supplied directory. */
    char copy[4096]; strcpy(copy, path);
    int directory = fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
    if (directory < 0) return -1;
    char *save = NULL, *part = strtok_r(copy, "/", &save);
    int fd = -1;
    for (;;) {
        if (!part) part = ".";
        if (!strcmp(part, "..")) { errno = EPERM; break; }
        char *next = strtok_r(NULL, "/", &save);
        fd = openat(directory, part, next ? O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
                                         : flags | O_CLOEXEC | O_NOFOLLOW, mode);
        if (fd < 0 || !next) break;
        close(directory); directory = fd; fd = -1; part = next;
    }
    int error = errno; close(directory); errno = error; return fd;
#endif
}
/* Lazy access must never follow a link outside a selected subtree. The ordinary
 * read/write API deliberately keeps its existing symlink semantics. */
int np_file_open_nofollow(int root_fd, const char *path, int flags, unsigned mode) {
    if (!path || path[0] != '/' || strlen(path) > 4095) { errno = EINVAL; return -1; }
    char copy[4096]; strcpy(copy, path);
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { errno = EINVAL; return -1; }
    }
#ifdef __linux__
    struct open_how how = {.flags = (uint64_t)(flags | O_CLOEXEC | O_NOFOLLOW), .mode = mode,
                          .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_SYMLINKS};
#ifdef NP_FILE_TEST_OPENAT2_ENOSYS
    (void)how;
    int fd = -1; errno = ENOSYS;
#else
    int fd = (int)syscall(SYS_openat2, root_fd, path, &how, sizeof(how));
#endif
    if (fd >= 0 || errno != ENOSYS) return fd;
    struct stat supplied, actual;
    if (fstat(root_fd, &supplied) < 0 || stat("/", &actual) < 0) return -1;
    if (supplied.st_dev != actual.st_dev || supplied.st_ino != actual.st_ino) { errno = ENOSYS; return -1; }
#endif
    strcpy(copy, path); save = NULL;
    char *part = strtok_r(copy, "/", &save);
    int directory = fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
    if (directory < 0) return -1;
    int result = -1;
    for (;;) {
        char *next = part ? strtok_r(NULL, "/", &save) : NULL;
        result = openat(directory, part ? part : ".", next ? O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY
                        : flags | O_CLOEXEC | O_NOFOLLOW, mode);
        if (result < 0 || !next) break;
        close(directory); directory = result; result = -1; part = next;
    }
    int error = errno; close(directory); errno = error; return result;
}
