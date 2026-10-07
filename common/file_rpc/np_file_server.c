#define _GNU_SOURCE
#include "np_file_rpc.h"
#include <dirent.h>
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
static int metadata(int socket, const struct stat *st) {
    unsigned char bytes[28];
    np_file_put32(bytes, (uint32_t)st->st_mode); np_file_put32(bytes + 4, st->st_uid);
    np_file_put32(bytes + 8, st->st_gid); np_file_put64(bytes + 12, (uint64_t)st->st_size);
    np_file_put64(bytes + 20, (uint64_t)st->st_mtime);
    return np_file_send(socket, NP_FILE_METADATA, 0, 0, bytes, sizeof(bytes));
}
static void revision(unsigned char bytes[NP_FILE_REVISION], const struct stat *st) {
    np_file_put64(bytes, (uint64_t)st->st_dev); np_file_put64(bytes + 8, (uint64_t)st->st_ino);
    np_file_put64(bytes + 16, (uint64_t)st->st_size);
#ifdef __APPLE__
    np_file_put64(bytes + 24, (uint64_t)st->st_mtimespec.tv_sec);
    np_file_put32(bytes + 32, (uint32_t)st->st_mtimespec.tv_nsec);
    np_file_put64(bytes + 36, (uint64_t)st->st_ctimespec.tv_sec);
    np_file_put32(bytes + 44, (uint32_t)st->st_ctimespec.tv_nsec);
#else
    np_file_put64(bytes + 24, (uint64_t)st->st_mtim.tv_sec);
    np_file_put32(bytes + 32, (uint32_t)st->st_mtim.tv_nsec);
    np_file_put64(bytes + 36, (uint64_t)st->st_ctim.tv_sec);
    np_file_put32(bytes + 44, (uint32_t)st->st_ctim.tv_nsec);
#endif
}
/* Lazy access must never follow a link outside a selected subtree. The ordinary
 * read/write API deliberately keeps its existing symlink semantics. */
static int open_snapshot(int root_fd, const char *path) {
    if (!path || path[0] != '/' || strlen(path) > 4095) { errno = EINVAL; return -1; }
    char copy[4096]; strcpy(copy, path);
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { errno = EINVAL; return -1; }
    }
#ifdef __linux__
    struct open_how how = {.flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW,
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
        result = openat(directory, part ? part : ".", O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW |
                        (next ? O_DIRECTORY : 0), 0);
        if (result < 0 || !next) break;
        close(directory); directory = result; result = -1; part = next;
    }
    int error = errno; close(directory); errno = error; return result;
}
static int snapshot_metadata(int socket, const struct stat *st) {
    unsigned char bytes[28 + NP_FILE_REVISION];
    np_file_put32(bytes, (uint32_t)st->st_mode); np_file_put32(bytes + 4, st->st_uid);
    np_file_put32(bytes + 8, st->st_gid); np_file_put64(bytes + 12, (uint64_t)st->st_size);
    np_file_put64(bytes + 20, (uint64_t)st->st_mtime); revision(bytes + 28, st);
    return np_file_send(socket, NP_FILE_METADATA, 0, 0, bytes, sizeof(bytes));
}
static int directory_stream(int socket, int fd, unsigned char *bytes) {
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); return -1; }
    int result = -1;
    struct dirent *pending = NULL;
    for (;;) {
        size_t length = 0;
        int error = 0;
        for (;;) {
            errno = 0;
            struct dirent *entry = pending ? pending : readdir(directory);
            pending = NULL;
            if (!entry) { error = errno; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            size_t size = strlen(entry->d_name);
            if (size > UINT16_MAX) { error = ENAMETOOLONG; break; }
            if (length + 3 + size > NP_FILE_CHUNK) { pending = entry; break; }
            bytes[length++] = entry->d_type;
            bytes[length++] = (unsigned char)size; bytes[length++] = (unsigned char)(size >> 8);
            memcpy(bytes + length, entry->d_name, size); length += size;
        }
        if (error) { errno = error; break; }
        if (!length) { result = np_file_send(socket, NP_FILE_END, 0, 0, NULL, 0); break; }
        if (np_file_send(socket, NP_FILE_ENTRIES, 0, 0, bytes, length) < 0) break;
    }
    int error = errno; closedir(directory); errno = error; return result;
}
int np_file_serve(int socket, int root_fd, int read_only) {
    /* musl's default pthread stack is 128 KiB. Keep one bounded frame on the
     * heap and reuse its payload for directory batches; nested stream calls
     * must not stack two 64 KiB buffers and crash the whole guest service. */
    struct np_file_frame *request = malloc(sizeof(*request));
    if (!request) { np_file_send(socket, NP_FILE_END, 0, ENOMEM, NULL, 0); errno = ENOMEM; return -1; }
    if (np_file_receive(socket, request) < 0) { int error = errno; free(request); errno = error; return -1; }
    int result = -1, fd = -1;
    if (request->status || (request->type > NP_FILE_WRITE && request->type != NP_FILE_MKDIR &&
        request->type != NP_FILE_SNAPSHOT && request->type != NP_FILE_RANGE && request->type != NP_FILE_DIRECTORY) || request->length < 5) {
        errno = EPROTO; goto done;
    }
    uint32_t length = np_file_u32(request->data);
    unsigned suffix = request->type == NP_FILE_WRITE ? 4u : request->type == NP_FILE_RANGE ? 12u + NP_FILE_REVISION : 0u;
    if (!length || length > 4095 || request->length != 4 + length + suffix ||
        memchr(request->data + 4, 0, length)) { errno = EINVAL; goto done; }
    char path[4096]; memcpy(path, request->data + 4, length); path[length] = 0;
    if (request->type == NP_FILE_SNAPSHOT || request->type == NP_FILE_RANGE || request->type == NP_FILE_DIRECTORY) {
        fd = open_snapshot(root_fd, path);
        if (fd < 0) goto done;
        struct stat before, after;
        if (fstat(fd, &before) < 0) goto done;
        if (!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode)) { errno = EINVAL; goto done; }
        if (request->type == NP_FILE_SNAPSHOT) { result = snapshot_metadata(socket, &before); goto done; }
        if (request->type == NP_FILE_DIRECTORY) {
            if (!S_ISDIR(before.st_mode)) { errno = ENOTDIR; goto done; }
            if (metadata(socket, &before) < 0) goto done;
            int directory_fd = fd; fd = -1;
            result = directory_stream(socket, directory_fd, request->data); goto done;
        }
        if (!S_ISREG(before.st_mode)) { errno = EISDIR; goto done; }
        unsigned char token[NP_FILE_REVISION]; revision(token, &before);
        uint64_t offset = np_file_u64(request->data + 4 + length);
        uint32_t count = np_file_u32(request->data + 12 + length);
        if (offset > INT64_MAX || count > NP_FILE_RANGE_MAX || count > INT64_MAX - offset) { errno = EINVAL; goto done; }
        if (memcmp(token, request->data + 16 + length, NP_FILE_REVISION)) { errno = NP_FILE_STALE; goto done; }
        uint64_t total = 0;
        while (total < count) {
            size_t size = count - total < NP_FILE_CHUNK ? (size_t)(count - total) : NP_FILE_CHUNK;
            ssize_t received;
            do { received = pread(fd, request->data, size, (off_t)(offset + total)); } while (received < 0 && errno == EINTR);
            if (received < 0) goto done;
            if (!received) break;
            if (np_file_send(socket, NP_FILE_DATA, 0, 0, request->data, (size_t)received) < 0) goto done;
            total += (uint64_t)received;
        }
        if (fstat(fd, &after) < 0) goto done;
        unsigned char final[NP_FILE_REVISION]; revision(final, &after);
        if (memcmp(token, final, NP_FILE_REVISION)) { errno = NP_FILE_STALE; goto done; }
        unsigned char end[8]; np_file_put64(end, total);
        result = np_file_send(socket, NP_FILE_END, 0, 0, end, sizeof(end));
    } else if (request->type == NP_FILE_MKDIR) {
        if (read_only) { errno = EROFS; goto done; }
        char *name = strrchr(path, '/');
        if (!name || !name[1] || !strcmp(name + 1, ".") || !strcmp(name + 1, "..")) { errno = EINVAL; goto done; }
        *name++ = 0;
        fd = np_file_open(root_fd, *path ? path : "/", O_RDONLY | O_DIRECTORY, 0);
        if (fd < 0 || mkdirat(fd, name, 0700) < 0) goto done;
        result = np_file_send(socket, NP_FILE_END, 0, 0, NULL, 0);
    } else if (request->type == NP_FILE_WRITE) {
        if (read_only) { errno = EROFS; goto done; }
        unsigned mode = np_file_u32(request->data + 4 + length);
        if (mode & ~07777u) { errno = EINVAL; goto done; }
        int flags = O_WRONLY | O_CREAT | O_NONBLOCK;
        if (!(request->flags & NP_FILE_REPLACE)) flags |= O_EXCL;
        fd = np_file_open(root_fd, path, flags, mode);
        if (fd < 0) goto done;
        struct stat st;
        if (fstat(fd, &st) < 0) goto done;
        if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto done; }
        if (ftruncate(fd, 0) < 0 || metadata(socket, &st) < 0) goto done;
        uint64_t received;
        if (np_file_receive_stream(socket, fd, UINT64_MAX, &received) < 0 ||
            fchmod(fd, mode) < 0 || fsync(fd) < 0) goto done;
        if (close(fd) < 0) { fd = -1; goto done; }
        fd = -1;
        unsigned char end[8]; np_file_put64(end, received);
        result = np_file_send(socket, NP_FILE_END, 0, 0, end, sizeof(end));
    } else {
#ifdef __linux__
        int flags = request->type == NP_FILE_STAT ? O_PATH | O_NOFOLLOW : O_RDONLY | O_NONBLOCK;
#else
        int flags = O_RDONLY | O_NONBLOCK;
#endif
        fd = np_file_open(root_fd, path, flags, 0);
        if (fd < 0) goto done;
        struct stat st;
        if (fstat(fd, &st) < 0) goto done;
        if (request->type == NP_FILE_STAT) { result = metadata(socket, &st); goto done; }
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) { errno = EINVAL; goto done; }
        if (request->type == NP_FILE_LIST && !S_ISDIR(st.st_mode)) { errno = ENOTDIR; goto done; }
        if (metadata(socket, &st) < 0) goto done;
        if (S_ISDIR(st.st_mode)) { int directory_fd = fd; fd = -1; result = directory_stream(socket, directory_fd, request->data); }
        else {
            /* send_stream already sends its terminal status, including a
             * source read failure. Do not append a second END on that path. */
            result = np_file_send_stream(socket, fd, UINT64_MAX);
            int error = errno; close(fd); free(request); errno = error;
            return result;
        }
    }
done:;
    int error = errno ? errno : EIO;
    if (fd >= 0) close(fd);
    free(request);
    if (result < 0) np_file_send(socket, NP_FILE_END, 0, (uint32_t)error, NULL, 0);
    errno = error;
    return result;
}
