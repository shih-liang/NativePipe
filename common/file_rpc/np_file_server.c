#define _GNU_SOURCE
#include "np_file_rpc.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
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
static int open_nofollow(int root_fd, const char *path, int flags, unsigned mode) {
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
static int open_snapshot(int root_fd, const char *path) {
    return open_nofollow(root_fd, path, O_RDONLY | O_NONBLOCK, 0);
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

static int valid_path(const char *path) {
    if (!path || path[0] != '/' || strlen(path) > 4095) return 0;
    const char *part = path;
    while (*part) {
        while (*part == '/') part++;
        size_t length = strcspn(part, "/");
        if ((length == 1 && part[0] == '.') || (length == 2 && !memcmp(part, "..", 2))) return 0;
        part += length;
    }
    return 1;
}
/* The worker runs as the logged-in Linux user. Never substitute the host's
 * username or a root-owned home guessed by the management process. */
static int user_home(char home[4096]) {
    const char *environment = getenv("HOME");
    if (valid_path(environment)) { strcpy(home, environment); return 0; }
    struct passwd entry, *found = NULL;
    size_t length = 8192;
    char *buffer = NULL;
    int error;
    do {
        free(buffer); buffer = malloc(length);
        if (!buffer) return -1;
        error = getpwuid_r(geteuid(), &entry, buffer, length, &found);
        length *= 2;
    } while (error == ERANGE && length <= 65536);
    if (!error && found && valid_path(found->pw_dir)) strcpy(home, found->pw_dir);
    else error = error ? error : ENOENT;
    free(buffer);
    if (error) { errno = error; return -1; }
    return 0;
}
static int browse_stream(int socket, int fd, unsigned char *bytes) {
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); return -1; }
    int result = -1;
    uint64_t count = 0;
    struct dirent *pending = NULL;
    for (;;) {
        size_t length = 0;
        int error = 0, finished = 0;
        for (;;) {
            errno = 0;
            struct dirent *entry = pending ? pending : readdir(directory);
            pending = NULL;
            if (!entry) { error = errno; finished = 1; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            size_t size = strlen(entry->d_name);
            if (!size || size > 255) { error = ENAMETOOLONG; break; }
            if (length + 30 + size > NP_FILE_CHUNK) { pending = entry; break; }
            struct stat st;
            if (fstatat(dirfd(directory), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
                if (errno == ENOENT) continue; /* Another process removed this entry. */
                error = errno; break;
            }
            bytes[length] = (unsigned char)size; bytes[length + 1] = (unsigned char)(size >> 8);
            np_file_put32(bytes + length + 2, (uint32_t)st.st_mode);
            np_file_put32(bytes + length + 6, st.st_uid); np_file_put32(bytes + length + 10, st.st_gid);
            np_file_put64(bytes + length + 14, (uint64_t)st.st_size);
            np_file_put64(bytes + length + 22, (uint64_t)st.st_mtime);
            memcpy(bytes + length + 30, entry->d_name, size);
            length += 30 + size; count++;
        }
        if (error) { errno = error; break; }
        if (length && np_file_send(socket, NP_FILE_ENTRIES, 0, 0, bytes, length) < 0) break;
        if (finished) {
            unsigned char end[8]; np_file_put64(end, count);
            result = np_file_send(socket, NP_FILE_END, 0, 0, end, sizeof(end)); break;
        }
    }
    int error = errno; closedir(directory); errno = error; return result;
}
#define STAGING_PREFIX ".nativepipe-upload-"
#define STAGING_MARKER ".nativepipe-owner"
#define STAGING_PAYLOAD ".payload"
#define STAGING_MARKER_SIZE (8 + NP_FILE_STAGING_TOKEN + 16)
static const unsigned char staging_magic[8] = {'N','P','U','F',1,0,0,0};
static int staging_name(const char *name) {
    const size_t prefix = sizeof(STAGING_PREFIX) - 1;
    if (strlen(name) != prefix + 36 || memcmp(name, STAGING_PREFIX, prefix)) return 0;
    for (unsigned i = 0; i < 36; i++) {
        unsigned char c = (unsigned char)name[prefix + i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return 0; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}
/* Split only validated absolute paths, and pin their parents without links. */
static int path_parent(int root, char path[4096], char **name) {
    if (!valid_path(path)) { errno = EINVAL; return -1; }
    char *separator = strrchr(path, '/');
    if (!separator || !separator[1]) { errno = EINVAL; return -1; }
    *separator = 0; *name = separator + 1;
    if (strlen(*name) > 255) { errno = ENAMETOOLONG; return -1; }
    return open_nofollow(root, *path ? path : "/", O_RDONLY | O_DIRECTORY, 0);
}
static int same_inode(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}
static int owned_staging(int parent, const char *name, const unsigned char *token) {
    if (!staging_name(name)) { errno = EINVAL; return -1; }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat directory, marker;
    int owner = -1, error = EACCES;
    unsigned char bytes[STAGING_MARKER_SIZE];
    if (fstat(fd, &directory) < 0) { error = errno; goto fail; }
    if (!S_ISDIR(directory.st_mode) || directory.st_uid != geteuid() || (directory.st_mode & 0777) != 0700) goto fail;
    owner = openat(fd, STAGING_MARKER, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (owner < 0) { error = errno; goto fail; }
    if (fstat(owner, &marker) < 0) { error = errno; goto fail; }
    if (!S_ISREG(marker.st_mode) || marker.st_uid != geteuid() || (marker.st_mode & 0777) != 0600 ||
        marker.st_nlink != 1 || marker.st_size != STAGING_MARKER_SIZE) goto fail;
    ssize_t received;
    do { received = read(owner, bytes, sizeof(bytes)); } while (received < 0 && errno == EINTR);
    if (received < 0) { error = errno; goto fail; }
    if (received != sizeof(bytes) || memcmp(bytes, staging_magic, sizeof(staging_magic)) ||
        memcmp(bytes + 8, token, NP_FILE_STAGING_TOKEN) ||
        np_file_u64(bytes + 8 + NP_FILE_STAGING_TOKEN) != (uint64_t)directory.st_dev ||
        np_file_u64(bytes + 16 + NP_FILE_STAGING_TOKEN) != (uint64_t)directory.st_ino) goto fail;
    close(owner); return fd;
fail:
    if (owner >= 0) close(owner);
    close(fd); errno = error; return -1;
}
static int discard_contents(int fd, unsigned depth) {
    if (depth > 64) { errno = ELOOP; return -1; }
    int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) return -1;
    DIR *directory = fdopendir(duplicate);
    if (!directory) { int error = errno; close(duplicate); errno = error; return -1; }
    int result = -1;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (!errno) result = 0; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || (!depth && !strcmp(entry->d_name, STAGING_MARKER))) continue;
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) break;
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) break;
            struct stat pinned;
            int child_result = fstat(child, &pinned);
            if (!child_result && !same_inode(&st, &pinned)) { errno = ESTALE; child_result = -1; }
            if (!child_result) child_result = discard_contents(child, depth + 1);
            int error = errno; close(child); errno = error;
            if (child_result < 0 || unlinkat(fd, entry->d_name, AT_REMOVEDIR) < 0) break;
        } else if (unlinkat(fd, entry->d_name, 0) < 0) break;
    }
    int error = errno; closedir(directory); errno = error; return result;
}
static int discard_staging(int parent, const char *name, int fd) {
    if (discard_contents(fd, 0) < 0) return -1;
    struct stat pinned, current;
    if (fstat(fd, &pinned) < 0 || fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW) < 0) return -1;
    if (!same_inode(&pinned, &current)) { errno = ESTALE; return -1; }
    if (unlinkat(fd, STAGING_MARKER, 0) < 0) return -1;
    return unlinkat(parent, name, AT_REMOVEDIR);
}
static int create_staging(int socket, int root, char path[4096], const unsigned char *token) {
    char *name;
    int parent = path_parent(root, path, &name);
    if (parent < 0) return -1;
    int fd = -1, marker = -1, created = 0, result = -1;
    unsigned char bytes[STAGING_MARKER_SIZE];
    if (!staging_name(name)) { errno = EINVAL; goto done; }
    memcpy(bytes + 8, token, NP_FILE_STAGING_TOKEN);
    if (mkdirat(parent, name, 0700) < 0) goto done;
    created = 1;
    fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) goto done;
    struct stat st;
    if (fstat(fd, &st) < 0 || fchmod(fd, 0700) < 0) goto done;
    memcpy(bytes, staging_magic, 8);
    np_file_put64(bytes + 8 + NP_FILE_STAGING_TOKEN, (uint64_t)st.st_dev);
    np_file_put64(bytes + 16 + NP_FILE_STAGING_TOKEN, (uint64_t)st.st_ino);
    marker = openat(fd, STAGING_MARKER, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (marker < 0) goto done;
    ssize_t written;
    do { written = write(marker, bytes, sizeof(bytes)); } while (written < 0 && errno == EINTR);
    if (written != sizeof(bytes)) { if (written >= 0) errno = EIO; goto done; }
    if (fchmod(marker, 0600) < 0 || fsync(marker) < 0) goto done;
    result = np_file_send(socket, NP_FILE_METADATA, 0, 0, bytes + 8, NP_FILE_STAGING_TOKEN);
done:;
    int error = errno;
    if (marker >= 0) close(marker);
    if (result < 0 && created) {
        if (fd >= 0) unlinkat(fd, STAGING_MARKER, 0);
        unlinkat(parent, name, AT_REMOVEDIR);
    }
    if (fd >= 0) close(fd);
    close(parent); errno = error; return result;
}
static int publish_staging(int root, char path[4096], const unsigned char *token, char destination[4096]) {
    char *name, *target;
    int parent = path_parent(root, path, &name);
    if (parent < 0) return -1;
    int fd = -1, destination_parent = -1, result = -1;
    fd = owned_staging(parent, name, token);
    if (fd < 0) goto done;
    destination_parent = path_parent(root, destination, &target);
    if (destination_parent < 0) goto done;
    struct stat a, b, payload;
    if (fstat(parent, &a) < 0 || fstat(destination_parent, &b) < 0) goto done;
    if (!same_inode(&a, &b)) { errno = EINVAL; goto done; }
    if (fstatat(fd, STAGING_PAYLOAD, &payload, AT_SYMLINK_NOFOLLOW) < 0) goto done;
    if (!S_ISREG(payload.st_mode) && !S_ISDIR(payload.st_mode)) { errno = EINVAL; goto done; }
#ifdef __APPLE__
    result = renameatx_np(fd, STAGING_PAYLOAD, destination_parent, target, RENAME_EXCL);
#elif defined(__linux__)
    result = (int)syscall(SYS_renameat2, fd, STAGING_PAYLOAD, destination_parent, target, 1u /* RENAME_NOREPLACE */);
#else
    errno = ENOTSUP;
#endif
    /* Rename is the commit point. A cleanup failure must not report a failed
     * upload after the completed file has become visible to its recipient. */
    if (!result) {
        unlinkat(fd, STAGING_MARKER, 0);
        unlinkat(parent, name, AT_REMOVEDIR);
    }
done:;
    int error = errno;
    if (destination_parent >= 0) close(destination_parent);
    if (fd >= 0) close(fd);
    close(parent); errno = error; return result;
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
        request->type != NP_FILE_SNAPSHOT && request->type != NP_FILE_RANGE && request->type != NP_FILE_DIRECTORY &&
        request->type != NP_FILE_BROWSE && request->type != NP_FILE_CREATE_STAGING &&
        request->type != NP_FILE_PUBLISH_STAGING && request->type != NP_FILE_DISCARD_STAGING) || request->length < 4) {
        errno = EPROTO; goto done;
    }
    uint32_t length = np_file_u32(request->data);
    unsigned suffix = request->type == NP_FILE_WRITE ? 4u : request->type == NP_FILE_RANGE ? 12u + NP_FILE_REVISION :
        (request->type == NP_FILE_DISCARD_STAGING || request->type == NP_FILE_CREATE_STAGING) ? NP_FILE_STAGING_TOKEN : 0u;
    if (length > 4095 || (!length && request->type != NP_FILE_BROWSE) || request->length < 4 + length ||
        memchr(request->data + 4, 0, length)) { errno = EINVAL; goto done; }
    char destination[4096];
    if (request->type == NP_FILE_PUBLISH_STAGING) {
        if (request->length < 4 + length + NP_FILE_STAGING_TOKEN + 4) { errno = EINVAL; goto done; }
        const unsigned char *target = request->data + 4 + length + NP_FILE_STAGING_TOKEN;
        uint32_t size = np_file_u32(target);
        if (!size || size > 4095 || request->length != 4 + length + NP_FILE_STAGING_TOKEN + 4 + size ||
            memchr(target + 4, 0, size)) { errno = EINVAL; goto done; }
        memcpy(destination, target + 4, size); destination[size] = 0;
    } else if (request->length != 4 + length + suffix) { errno = EINVAL; goto done; }
    char path[4096]; memcpy(path, request->data + 4, length); path[length] = 0;
    if (request->type == NP_FILE_BROWSE) {
        char home[4096];
        if (user_home(home) < 0) goto done;
        if (!length) strcpy(path, home);
        fd = open_snapshot(root_fd, path);
        if (fd < 0) goto done;
        struct stat st;
        if (fstat(fd, &st) < 0) goto done;
        if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; goto done; }
        size_t path_size = strlen(path), home_size = strlen(home);
        np_file_put32(request->data, (uint32_t)path_size); np_file_put32(request->data + 4, (uint32_t)home_size);
        memcpy(request->data + 8, path, path_size); memcpy(request->data + 8 + path_size, home, home_size);
        if (np_file_send(socket, NP_FILE_METADATA, 0, 0, request->data, 8 + path_size + home_size) < 0) goto done;
        int directory_fd = fd; fd = -1;
        result = browse_stream(socket, directory_fd, request->data); goto done;
    } else if (request->type == NP_FILE_CREATE_STAGING || request->type == NP_FILE_PUBLISH_STAGING ||
               request->type == NP_FILE_DISCARD_STAGING) {
        if (read_only) { errno = EROFS; goto done; }
        if (request->type == NP_FILE_CREATE_STAGING) { result = create_staging(socket, root_fd, path, request->data + 4 + length); goto done; }
        const unsigned char *token = request->data + 4 + length;
        if (request->type == NP_FILE_PUBLISH_STAGING) {
            if (publish_staging(root_fd, path, token, destination) < 0) goto done;
        } else {
            char *name;
            int parent = path_parent(root_fd, path, &name);
            if (parent < 0) goto done;
            fd = owned_staging(parent, name, token);
            int cleanup = fd < 0 ? -1 : discard_staging(parent, name, fd);
            int error = errno; close(parent); errno = error;
            if (cleanup < 0) goto done;
        }
        result = np_file_send(socket, NP_FILE_END, 0, 0, NULL, 0); goto done;
    }
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
        fd = (request->flags & NP_FILE_NOFOLLOW) ? open_nofollow(root_fd, *path ? path : "/", O_RDONLY | O_DIRECTORY, 0)
                                               : np_file_open(root_fd, *path ? path : "/", O_RDONLY | O_DIRECTORY, 0);
        if (fd < 0 || mkdirat(fd, name, 0700) < 0) goto done;
        result = np_file_send(socket, NP_FILE_END, 0, 0, NULL, 0);
    } else if (request->type == NP_FILE_WRITE) {
        if (read_only) { errno = EROFS; goto done; }
        unsigned mode = np_file_u32(request->data + 4 + length);
        if (mode & ~07777u) { errno = EINVAL; goto done; }
        int flags = O_WRONLY | O_CREAT | O_NONBLOCK;
        if (!(request->flags & NP_FILE_REPLACE)) flags |= O_EXCL;
        fd = (request->flags & NP_FILE_NOFOLLOW) ? open_nofollow(root_fd, path, flags, mode)
                                               : np_file_open(root_fd, path, flags, mode);
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
