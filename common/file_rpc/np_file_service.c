#define _GNU_SOURCE
#include "np_file_rpc.h"
#include "np_file_session.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

/* Each connection is one NPFR session. Requests run in order on the thread
 * serving the connection, never on the session's reader; handles belong to
 * the session and close with it. */

#define ERROR (errno ? errno : EIO)

static void metadata(unsigned char bytes[NP_FILE_METADATA], const struct stat *st) {
    np_file_put32(bytes, (uint32_t)st->st_mode); np_file_put32(bytes + 4, st->st_uid);
    np_file_put32(bytes + 8, st->st_gid); np_file_put64(bytes + 12, (uint64_t)st->st_size);
    np_file_put64(bytes + 20, (uint64_t)st->st_mtime);
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

static int valid_path(const char *path) {
    if (!path || path[0] != '/' || strlen(path) > NP_FILE_PATH_MAX) return 0;
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
        if (!buffer) return ENOMEM;
        error = getpwuid_r(geteuid(), &entry, buffer, length, &found);
        length *= 2;
    } while (error == ERANGE && length <= 65536);
    if (!error && found && valid_path(found->pw_dir)) strcpy(home, found->pw_dir);
    else error = error ? error : ENOENT;
    free(buffer);
    return error;
}
/* A path is path_length:u32 then its bytes. */
static int take_path(const unsigned char *body, size_t length, size_t *offset, char path[4096], int allow_empty) {
    if (length - *offset < 4) return EINVAL;
    uint32_t size = np_file_u32(body + *offset);
    *offset += 4;
    if (size > NP_FILE_PATH_MAX || size > length - *offset || (!size && !allow_empty) ||
        memchr(body + *offset, 0, size)) return EINVAL;
    memcpy(path, body + *offset, size); path[size] = 0;
    *offset += size;
    if (size && path[0] != '/') return EINVAL;
    return 0;
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
    return np_file_open_nofollow(root, *path ? path : "/", O_RDONLY | O_DIRECTORY, 0);
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
static int create_staging(int root, char path[4096], const unsigned char *token) {
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
    result = 0;
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

struct handle {
    uint64_t id;          /* 0 when free */
    int fd;               /* a file; a directory's descriptor belongs to `directory` */
    DIR *directory;
    unsigned flags, mode;
    /* A directory entry that did not fit the previous READ. */
    int pending;
    unsigned char pending_type;
    char pending_name[256];
};
struct job {
    struct job *next;
    struct np_file2_stream *stream;
    uint16_t opcode;
    size_t length;
    unsigned char body[];
};
struct connection {
    int root, read_only;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    struct job *head, *tail;
    int closed;
    struct handle handles[NP_FILE_HANDLES];
    uint64_t next_handle;
    unsigned char *buffer;  /* NP_FILE_IO_MAX bytes */
    struct np_file2_message *message;
};

static void on_request(void *context, struct np_file2_stream *stream, uint16_t opcode,
                       const void *body, size_t length) {
    struct connection *connection = context;
    struct job *job = malloc(sizeof(*job) + length);
    if (!job) { np_file2_stream_release(stream); return; }
    job->next = NULL; job->stream = stream; job->opcode = opcode; job->length = length;
    memcpy(job->body, body, length);
    pthread_mutex_lock(&connection->lock);
    if (connection->tail) connection->tail->next = job; else connection->head = job;
    connection->tail = job;
    pthread_cond_broadcast(&connection->changed);
    pthread_mutex_unlock(&connection->lock);
}
static void on_closed(void *context, int error) {
    (void)error;
    struct connection *connection = context;
    pthread_mutex_lock(&connection->lock);
    connection->closed = 1;
    pthread_cond_broadcast(&connection->changed);
    pthread_mutex_unlock(&connection->lock);
}

/* Answers with a body and no DATA, or with DATA after an empty body. */
static int reply(struct np_file2_stream *stream, const void *body, size_t length) {
    np_file2_respond(stream, 0, body, length, 1);
    return 0;
}
static int reply_data(struct np_file2_stream *stream, const void *data, size_t length) {
    if (np_file2_respond(stream, 0, NULL, 0, !length) == 0 && length) np_file2_send(stream, data, length, 1);
    return 0;
}
static int write_all(int fd, const unsigned char *bytes, size_t size) {
    while (size) {
        ssize_t n = write(fd, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? errno : EIO;
        bytes += n; size -= (size_t)n;
    }
    return 0;
}

static struct handle *find_handle(struct connection *connection, const unsigned char *body) {
    uint64_t id = np_file_u64(body);
    for (unsigned i = 0; id && i < NP_FILE_HANDLES; i++)
        if (connection->handles[i].id == id) return &connection->handles[i];
    return NULL;
}
static int close_handle(struct handle *handle) {
    int error = 0;
    if (handle->directory) { closedir(handle->directory); handle->directory = NULL; }
    if (handle->fd >= 0) {
        if (handle->flags & NP_FILE_OPEN_WRITE) {
            if (fchmod(handle->fd, handle->mode) < 0 || fsync(handle->fd) < 0) error = ERROR;
        }
        if (close(handle->fd) < 0 && !error) error = ERROR;
        handle->fd = -1;
    }
    handle->id = 0; handle->pending = 0;
    return error;
}

static int op_stat(struct connection *connection, struct np_file2_stream *stream, const unsigned char *body, size_t length) {
    char path[4096]; size_t offset = 0;
    int error = take_path(body, length, &offset, path, 0);
    if (error) return error;
    if (offset != length) return EINVAL;
#ifdef __linux__
    int fd = np_file_open(connection->root, path, O_PATH | O_NOFOLLOW, 0);
#else
    int fd = np_file_open(connection->root, path, O_RDONLY | O_NONBLOCK, 0);
#endif
    if (fd < 0) return ERROR;
    struct stat st;
    error = fstat(fd, &st) < 0 ? ERROR : 0;
    close(fd);
    if (error) return error;
    unsigned char bytes[NP_FILE_METADATA]; metadata(bytes, &st);
    return reply(stream, bytes, sizeof(bytes));
}

static int op_open(struct connection *connection, struct np_file2_stream *stream, const unsigned char *body, size_t length) {
    if (length < 8) return EINVAL;
    unsigned flags = np_file_u32(body), mode = np_file_u32(body + 4);
    const unsigned known = NP_FILE_OPEN_WRITE | NP_FILE_REPLACE | NP_FILE_NOFOLLOW |
                           NP_FILE_OPEN_DIRECTORY | NP_FILE_OPEN_STAT | NP_FILE_OPEN_HOME;
    const unsigned listing = NP_FILE_OPEN_DIRECTORY | NP_FILE_OPEN_STAT | NP_FILE_OPEN_HOME;
    int write_mode = flags & NP_FILE_OPEN_WRITE;
    if ((flags & ~known) || (write_mode && (flags & listing)) || (!write_mode && (flags & NP_FILE_REPLACE)) ||
        (mode & ~07777u) || (!write_mode && mode)) return EINVAL;
    if (write_mode && connection->read_only) return EROFS;
    char path[4096], home[4096]; size_t offset = 8;
    int error = take_path(body, length, &offset, path, flags & NP_FILE_OPEN_HOME);
    if (error) return error;
    if (offset != length) return EINVAL;
    if (flags & NP_FILE_OPEN_HOME) {
        if ((error = user_home(home))) return error;
        if (!path[0]) strcpy(path, home);
    }
    struct handle *handle = NULL;
    for (unsigned i = 0; !handle && i < NP_FILE_HANDLES; i++)
        if (!connection->handles[i].id) handle = &connection->handles[i];
    if (!handle) return EMFILE;
    int open_flags = write_mode ? O_WRONLY | O_CREAT | O_NONBLOCK | ((flags & NP_FILE_REPLACE) ? 0 : O_EXCL)
                                : O_RDONLY | O_NONBLOCK;
    int fd = (flags & NP_FILE_NOFOLLOW) ? np_file_open_nofollow(connection->root, path, open_flags, mode)
                                        : np_file_open(connection->root, path, open_flags, mode);
    if (fd < 0) return ERROR;
    struct stat st;
    if (fstat(fd, &st) < 0) { error = ERROR; goto fail; }
    if (write_mode) {
        if (!S_ISREG(st.st_mode)) { error = EINVAL; goto fail; }
        if (ftruncate(fd, 0) < 0 || fstat(fd, &st) < 0) { error = ERROR; goto fail; }
    } else if ((flags & listing) && !S_ISDIR(st.st_mode)) { error = ENOTDIR; goto fail; }
    else if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) { error = EINVAL; goto fail; }
    handle->fd = -1; handle->directory = NULL; handle->pending = 0;
    if (S_ISDIR(st.st_mode)) {
        handle->directory = fdopendir(fd);
        if (!handle->directory) { error = ERROR; goto fail; }
    } else handle->fd = fd;
    handle->flags = flags; handle->mode = mode;
    handle->id = ++connection->next_handle;
    unsigned char reply_body[8 + NP_FILE_METADATA + 8 + 2 * NP_FILE_PATH_MAX];
    size_t size = 8 + NP_FILE_METADATA;
    np_file_put64(reply_body, handle->id); metadata(reply_body + 8, &st);
    if (flags & NP_FILE_OPEN_HOME) {
        size_t path_size = strlen(path), home_size = strlen(home);
        np_file_put32(reply_body + size, (uint32_t)path_size); np_file_put32(reply_body + size + 4, (uint32_t)home_size);
        memcpy(reply_body + size + 8, path, path_size); memcpy(reply_body + size + 8 + path_size, home, home_size);
        size += 8 + path_size + home_size;
    }
    return reply(stream, reply_body, size);
fail:
    close(fd);
    return error;
}

/* Fills the buffer with whole directory records: type:u8, name_length:u16,
 * name; or with OPEN_STAT, name_length:u16, 28 bytes of lstat metadata, name. */
static int read_directory(struct handle *handle, unsigned char *bytes, size_t capacity, size_t *filled) {
    int with_stat = handle->flags & NP_FILE_OPEN_STAT;
    size_t length = 0;
    for (;;) {
        unsigned char type; const char *name;
        if (handle->pending) { type = handle->pending_type; name = handle->pending_name; }
        else {
            errno = 0;
            struct dirent *entry = readdir(handle->directory);
            if (!entry) { if (errno) return errno; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            size_t size = strlen(entry->d_name);
            if (!size || size > 255) return ENAMETOOLONG;
            type = entry->d_type; name = entry->d_name;
            handle->pending_type = type; memcpy(handle->pending_name, name, size + 1);
            name = handle->pending_name;
        }
        size_t size = strlen(name), record = (with_stat ? 2 + NP_FILE_METADATA : 3) + size;
        if (length + record > capacity) {
            handle->pending = 1;
            if (!length) return EMSGSIZE;
            break;
        }
        handle->pending = 0;
        if (with_stat) {
            struct stat st;
            if (fstatat(dirfd(handle->directory), name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
                if (errno == ENOENT) continue; /* Another process removed this entry. */
                return ERROR;
            }
            bytes[length] = (unsigned char)size; bytes[length + 1] = (unsigned char)(size >> 8);
            metadata(bytes + length + 2, &st);
            memcpy(bytes + length + 2 + NP_FILE_METADATA, name, size);
        } else {
            bytes[length] = type;
            bytes[length + 1] = (unsigned char)size; bytes[length + 2] = (unsigned char)(size >> 8);
            memcpy(bytes + length + 3, name, size);
        }
        length += record;
    }
    *filled = length;
    return 0;
}

static int op_read(struct connection *connection, struct np_file2_stream *stream, const unsigned char *body, size_t length) {
    if (length != 12) return EINVAL;
    struct handle *handle = find_handle(connection, body);
    uint32_t count = np_file_u32(body + 8);
    if (!handle || (handle->flags & NP_FILE_OPEN_WRITE)) return EBADF;
    if (!count || count > NP_FILE_IO_MAX) return EINVAL;
    size_t total = 0;
    if (handle->directory) {
        int error = read_directory(handle, connection->buffer, count, &total);
        if (error) return error;
    } else {
        /* Regular files are read to EOF, including size-zero procfs files. */
        while (total < count) {
            ssize_t n = read(handle->fd, connection->buffer + total, count - total);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return ERROR;
            if (!n) break;
            total += (size_t)n;
        }
    }
    return reply_data(stream, connection->buffer, total);
}

static int op_write(struct connection *connection, struct np_file2_stream *stream, const unsigned char *body, size_t length) {
    if (length != 8) return EINVAL;
    struct handle *handle = find_handle(connection, body);
    if (!handle || !(handle->flags & NP_FILE_OPEN_WRITE)) return EBADF;
    uint32_t written = 0;
    int result;
    while ((result = np_file2_receive(stream, connection->message)) == 1) {
        if (connection->message->type != NP_FILE2_DATA) return EPROTO;
        int error = write_all(handle->fd, connection->message->data, connection->message->length);
        if (error) return error;
        written += connection->message->length;
    }
    if (result < 0) return ERROR;
    unsigned char bytes[4]; np_file_put32(bytes, written);
    return reply(stream, bytes, sizeof(bytes));
}

static int op_close(struct connection *connection, struct np_file2_stream *stream, const unsigned char *body, size_t length) {
    if (length != 8) return EINVAL;
    struct handle *handle = find_handle(connection, body);
    if (!handle) return EBADF;
    int error = close_handle(handle);
    return error ? error : reply(stream, NULL, 0);
}

static int op_mkdir(struct connection *connection, struct np_file2_stream *stream, const unsigned char *body, size_t length) {
    if (length < 4) return EINVAL;
    unsigned flags = np_file_u32(body);
    if (flags & ~NP_FILE_NOFOLLOW) return EINVAL;
    if (connection->read_only) return EROFS;
    char path[4096]; size_t offset = 4;
    int error = take_path(body, length, &offset, path, 0);
    if (error) return error;
    if (offset != length) return EINVAL;
    char *name = strrchr(path, '/');
    if (!name || !name[1] || !strcmp(name + 1, ".") || !strcmp(name + 1, "..")) return EINVAL;
    *name++ = 0;
    int fd = (flags & NP_FILE_NOFOLLOW) ? np_file_open_nofollow(connection->root, *path ? path : "/", O_RDONLY | O_DIRECTORY, 0)
                                        : np_file_open(connection->root, *path ? path : "/", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return ERROR;
    error = mkdirat(fd, name, 0700) < 0 ? ERROR : 0;
    close(fd);
    return error ? error : reply(stream, NULL, 0);
}

/* Lazy access pins a no-link path and never follows a link out of a subtree. */
static int open_snapshot(struct connection *connection, const char *path, struct stat *st) {
    int fd = np_file_open_nofollow(connection->root, path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (fstat(fd, st) < 0) { int error = errno; close(fd); errno = error; return -1; }
    if (!S_ISREG(st->st_mode) && !S_ISDIR(st->st_mode)) { close(fd); errno = EINVAL; return -1; }
    return fd;
}

static int op_snapshot(struct connection *connection, struct np_file2_stream *stream, const unsigned char *body, size_t length) {
    char path[4096]; size_t offset = 0;
    int error = take_path(body, length, &offset, path, 0);
    if (error) return error;
    if (offset != length) return EINVAL;
    struct stat st;
    int fd = open_snapshot(connection, path, &st);
    if (fd < 0) return ERROR;
    close(fd);
    unsigned char bytes[NP_FILE_METADATA + NP_FILE_REVISION];
    metadata(bytes, &st); revision(bytes + NP_FILE_METADATA, &st);
    return reply(stream, bytes, sizeof(bytes));
}

static int op_range(struct connection *connection, struct np_file2_stream *stream, const unsigned char *body, size_t length) {
    if (length < 12 + NP_FILE_REVISION) return EINVAL;
    uint64_t offset = np_file_u64(body);
    uint32_t count = np_file_u32(body + 8);
    const unsigned char *expected = body + 12;
    char path[4096]; size_t position = 12 + NP_FILE_REVISION;
    int error = take_path(body, length, &position, path, 0);
    if (error) return error;
    if (position != length) return EINVAL;
    if (offset > INT64_MAX || count > NP_FILE_IO_MAX || count > INT64_MAX - offset) return EINVAL;
    struct stat before, after;
    int fd = open_snapshot(connection, path, &before);
    if (fd < 0) return ERROR;
    unsigned char current[NP_FILE_REVISION];
    size_t total = 0;
    if (!S_ISREG(before.st_mode)) { error = EISDIR; goto done; }
    revision(current, &before);
    if (memcmp(current, expected, NP_FILE_REVISION)) { error = NP_FILE_STALE; goto done; }
    while (total < count) {
        ssize_t n = pread(fd, connection->buffer + total, count - total, (off_t)(offset + total));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { error = ERROR; goto done; }
        if (!n) break;
        total += (size_t)n;
    }
    if (fstat(fd, &after) < 0) { error = ERROR; goto done; }
    revision(current, &after);
    /* The caller discards every byte of a range whose file changed meanwhile. */
    if (memcmp(current, expected, NP_FILE_REVISION)) error = NP_FILE_STALE;
done:
    close(fd);
    return error ? error : reply_data(stream, connection->buffer, total);
}

static int op_staging(struct connection *connection, struct np_file2_stream *stream, uint16_t opcode,
                      const unsigned char *body, size_t length) {
    if (connection->read_only) return EROFS;
    if (length < NP_FILE_STAGING_TOKEN) return EINVAL;
    const unsigned char *token = body;
    char path[4096], destination[4096]; size_t offset = NP_FILE_STAGING_TOKEN;
    int error = opcode == NP_FILE_PUBLISH_STAGING ? take_path(body, length, &offset, destination, 0) : 0;
    if (!error) error = take_path(body, length, &offset, path, 0);
    if (error) return error;
    if (offset != length) return EINVAL;
    if (opcode == NP_FILE_CREATE_STAGING) {
        if (create_staging(connection->root, path, token) < 0) return ERROR;
    } else if (opcode == NP_FILE_PUBLISH_STAGING) {
        if (publish_staging(connection->root, path, token, destination) < 0) return ERROR;
    } else {
        char *name;
        int parent = path_parent(connection->root, path, &name);
        if (parent < 0) return ERROR;
        int fd = owned_staging(parent, name, token);
        int cleanup = fd < 0 ? -1 : discard_staging(parent, name, fd);
        error = cleanup < 0 ? ERROR : 0;
        if (fd >= 0) close(fd);
        close(parent);
        if (error) return error;
    }
    return reply(stream, NULL, 0);
}

static void perform(struct connection *connection, struct job *job) {
    errno = 0;
    int error;
    switch (job->opcode) {
    case NP_FILE_STAT: error = op_stat(connection, job->stream, job->body, job->length); break;
    case NP_FILE_OPEN: error = op_open(connection, job->stream, job->body, job->length); break;
    case NP_FILE_READ: error = op_read(connection, job->stream, job->body, job->length); break;
    case NP_FILE_WRITE: error = op_write(connection, job->stream, job->body, job->length); break;
    case NP_FILE_CLOSE: error = op_close(connection, job->stream, job->body, job->length); break;
    case NP_FILE_MKDIR: error = op_mkdir(connection, job->stream, job->body, job->length); break;
    case NP_FILE_SNAPSHOT: error = op_snapshot(connection, job->stream, job->body, job->length); break;
    case NP_FILE_RANGE: error = op_range(connection, job->stream, job->body, job->length); break;
    case NP_FILE_CREATE_STAGING: case NP_FILE_PUBLISH_STAGING: case NP_FILE_DISCARD_STAGING:
        error = op_staging(connection, job->stream, job->opcode, job->body, job->length); break;
    default: error = ENOSYS; break;
    }
    if (!error) return;
    np_file2_respond(job->stream, (uint32_t)error, NULL, 0, 1);
    /* Releasing a stream the host is still sending on would cancel it, and a
     * cancellation discards this answer before the host reads it. Take the
     * rest of its data (at most one request's worth) first. */
    while (np_file2_receive(job->stream, connection->message) == 1) {}
}

int np_file_serve(int socket, int root_fd, int read_only) {
    struct connection *connection = calloc(1, sizeof(*connection));
    unsigned char *buffer = malloc(NP_FILE_IO_MAX);
    struct np_file2_message *message = malloc(sizeof(*message));
    if (!connection || !buffer || !message) {
        free(connection); free(buffer); free(message);
        close(socket); errno = ENOMEM; return -1;
    }
    connection->root = root_fd; connection->read_only = read_only;
    connection->buffer = buffer; connection->message = message;
    for (unsigned i = 0; i < NP_FILE_HANDLES; i++) connection->handles[i].fd = -1;
    pthread_mutex_init(&connection->lock, NULL);
    pthread_cond_init(&connection->changed, NULL);
    struct np_file2_handlers handlers = {connection, on_request, NULL, on_closed};
    struct np_file2_session *session = np_file2_session_start(socket, NP_FILE2_ROLE_GUEST, &handlers);
    int error = session ? 0 : errno;
    while (session) {
        pthread_mutex_lock(&connection->lock);
        while (!connection->head && !connection->closed) pthread_cond_wait(&connection->changed, &connection->lock);
        struct job *job = connection->head;
        if (job) { connection->head = job->next; if (!job->next) connection->tail = NULL; }
        int closed = connection->closed;
        pthread_mutex_unlock(&connection->lock);
        if (!job) break;
        if (!closed) perform(connection, job);
        np_file2_stream_release(job->stream);
        free(job);
    }
    if (session) np_file2_session_close(session);
    /* The reader has stopped; no request can arrive any more. */
    for (struct job *job = connection->head; job;) {
        struct job *next = job->next;
        np_file2_stream_release(job->stream); free(job); job = next;
    }
    for (unsigned i = 0; i < NP_FILE_HANDLES; i++)
        if (connection->handles[i].id) close_handle(&connection->handles[i]);
    pthread_cond_destroy(&connection->changed);
    pthread_mutex_destroy(&connection->lock);
    free(message); free(buffer); free(connection);
    errno = error;
    return error ? -1 : 0;
}

#define NP_FILE_CLIENTS 16
struct np_file_service {
    int listener, wake[2], clients[NP_FILE_CLIENTS];
    char *root;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t drained;
    unsigned active;
};
struct file_client { struct np_file_service *service; unsigned slot; };
static void *serve_client(void *data) {
    struct file_client *client = data;
    struct np_file_service *service = client->service;
    unsigned slot = client->slot;
    free(client);
    /* The session owns a duplicate; the slot keeps the original so stopping
     * can shut the socket down without racing a reused descriptor number. */
    int fd = service->clients[slot];
    int session = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    int root = open(service->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (session >= 0 && root >= 0) np_file_serve(session, root, 0);
    else if (session >= 0) close(session);
    if (root >= 0) close(root);
    pthread_mutex_lock(&service->lock);
    close(fd);
    service->clients[slot] = -1;
    service->active--;
    pthread_cond_broadcast(&service->drained);
    pthread_mutex_unlock(&service->lock);
    return NULL;
}
static void *listen_clients(void *data) {
    struct np_file_service *service = data;
    struct pollfd pollers[2] = {{.fd = service->listener, .events = POLLIN},
                                {.fd = service->wake[0], .events = POLLIN}};
    for (;;) {
        int ready = poll(pollers, 2, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || pollers[1].revents || (pollers[0].revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (!(pollers[0].revents & POLLIN)) continue;
#ifdef __linux__
        int fd = accept4(service->listener, NULL, NULL, SOCK_CLOEXEC);
#else
        int fd = accept(service->listener, NULL, NULL);
#endif
        if (fd < 0) continue;
        if (!np_file_peer_is_host(fd)) { close(fd); continue; }
        pthread_mutex_lock(&service->lock);
        unsigned slot = 0;
        while (slot < NP_FILE_CLIENTS && service->clients[slot] >= 0) slot++;
        struct file_client *client = slot < NP_FILE_CLIENTS ? malloc(sizeof(*client)) : NULL;
        pthread_t thread;
        if (client) {
            client->service = service; client->slot = slot;
            service->clients[slot] = fd;
            service->active++;
        }
        if (!client || pthread_create(&thread, NULL, serve_client, client) != 0) {
            if (client) { service->clients[slot] = -1; service->active--; free(client); }
            shutdown(fd, SHUT_RDWR); close(fd);
        } else pthread_detach(thread);
        pthread_mutex_unlock(&service->lock);
    }
    return NULL;
}
struct np_file_service *np_file_service_start(uint32_t port, const char *root) {
    struct np_file_service *service = calloc(1, sizeof(*service));
    if (!service) return NULL;
    service->listener = -1;
    service->root = strdup(root);
    if (!service->root) goto fail;
    service->listener = np_file_listen_vsock(port);
    if (service->listener < 0) goto fail;
#ifdef __linux__
    if (pipe2(service->wake, O_CLOEXEC) < 0) goto fail;
#else
    if (pipe(service->wake) < 0) goto fail;
#endif
    for (unsigned i = 0; i < NP_FILE_CLIENTS; i++) service->clients[i] = -1;
    int error = pthread_mutex_init(&service->lock, NULL);
    if (error) goto fail_pipe;
    error = pthread_cond_init(&service->drained, NULL);
    if (error) goto fail_mutex;
    error = pthread_create(&service->thread, NULL, listen_clients, service);
    if (!error) return service;
    pthread_cond_destroy(&service->drained);
fail_mutex:
    pthread_mutex_destroy(&service->lock);
fail_pipe:
    close(service->wake[0]); close(service->wake[1]);
    errno = error;
fail:;
    int saved = errno;
    if (service->listener >= 0) close(service->listener);
    free(service->root); free(service); errno = saved; return NULL;
}
void np_file_service_stop(struct np_file_service *service) {
    if (!service) return;
    /* This pipe has one owner and shutdown is one-shot. Closing its writer
     * wakes poll with HUP without a fallible/interrupted notification write. */
    close(service->wake[1]);
    pthread_join(service->thread, NULL);
    pthread_mutex_lock(&service->lock);
    for (unsigned i = 0; i < NP_FILE_CLIENTS; i++)
        if (service->clients[i] >= 0) shutdown(service->clients[i], SHUT_RDWR);
    while (service->active) pthread_cond_wait(&service->drained, &service->lock);
    pthread_mutex_unlock(&service->lock);
    close(service->listener); close(service->wake[0]);
    pthread_cond_destroy(&service->drained); pthread_mutex_destroy(&service->lock);
    free(service->root); free(service);
}
