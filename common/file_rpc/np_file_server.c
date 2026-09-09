#define _GNU_SOURCE
#include "np_file_rpc.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/openat2.h>
#include <linux/vm_sockets.h>
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
int np_file_open(int root_fd, const char *path, int flags, unsigned mode) {
    if (!path || path[0] != '/' || strlen(path) > 4095) { errno = EINVAL; return -1; }
#ifdef __linux__
    struct open_how how = {.flags = (uint64_t)(flags | O_CLOEXEC), .mode = mode,
                          .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS};
    return (int)syscall(SYS_openat2, root_fd, path, &how, sizeof(how));
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
static int directory_stream(int socket, int fd) {
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); return -1; }
    int result = -1;
    unsigned char bytes[NP_FILE_CHUNK];
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
            if (length + 3 + size > sizeof(bytes)) { pending = entry; break; }
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
    struct np_file_frame request;
    if (np_file_receive(socket, &request) < 0) return -1;
    int result = -1, fd = -1;
    if (request.status || (request.type > NP_FILE_WRITE && request.type != NP_FILE_MKDIR) || request.length < 5) {
        errno = EPROTO; goto done;
    }
    uint32_t length = np_file_u32(request.data);
    if (!length || length > 4095 || request.length != 4 + length + (request.type == NP_FILE_WRITE ? 4u : 0u) ||
        memchr(request.data + 4, 0, length)) { errno = EINVAL; goto done; }
    char path[4096]; memcpy(path, request.data + 4, length); path[length] = 0;
    if (request.type == NP_FILE_MKDIR) {
        if (read_only) { errno = EROFS; goto done; }
        char *name = strrchr(path, '/');
        if (!name || !name[1] || !strcmp(name + 1, ".") || !strcmp(name + 1, "..")) { errno = EINVAL; goto done; }
        *name++ = 0;
        fd = np_file_open(root_fd, *path ? path : "/", O_RDONLY | O_DIRECTORY, 0);
        if (fd < 0 || mkdirat(fd, name, 0700) < 0) goto done;
        result = np_file_send(socket, NP_FILE_END, 0, 0, NULL, 0);
    } else if (request.type == NP_FILE_WRITE) {
        if (read_only) { errno = EROFS; goto done; }
        unsigned mode = np_file_u32(request.data + 4 + length);
        if (mode & ~07777u) { errno = EINVAL; goto done; }
        int flags = O_WRONLY | O_CREAT | O_NONBLOCK;
        if (!(request.flags & NP_FILE_REPLACE)) flags |= O_EXCL;
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
        int flags = request.type == NP_FILE_STAT ? O_PATH | O_NOFOLLOW : O_RDONLY | O_NONBLOCK;
#else
        int flags = O_RDONLY | O_NONBLOCK;
#endif
        fd = np_file_open(root_fd, path, flags, 0);
        if (fd < 0) goto done;
        struct stat st;
        if (fstat(fd, &st) < 0) goto done;
        if (request.type == NP_FILE_STAT) { result = metadata(socket, &st); goto done; }
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) { errno = EINVAL; goto done; }
        if (request.type == NP_FILE_LIST && !S_ISDIR(st.st_mode)) { errno = ENOTDIR; goto done; }
        if (metadata(socket, &st) < 0) goto done;
        if (S_ISDIR(st.st_mode)) { int directory_fd = fd; fd = -1; result = directory_stream(socket, directory_fd); }
        else {
            /* send_stream already sends its terminal status, including a
             * source read failure. Do not append a second END on that path. */
            result = np_file_send_stream(socket, fd, UINT64_MAX);
            int error = errno; close(fd); errno = error;
            return result;
        }
    }
done:;
    int error = errno ? errno : EIO;
    if (fd >= 0) close(fd);
    if (result < 0) np_file_send(socket, NP_FILE_END, 0, (uint32_t)error, NULL, 0);
    errno = error;
    return result;
}
