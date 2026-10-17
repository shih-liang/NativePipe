#define _GNU_SOURCE
#undef NDEBUG
#include "np_file_rpc.h"
#include "np_file_session.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <unistd.h>

struct server { int socket, root, read_only, result; };
static void *serve(void *arg) {
    struct server *s = arg;
    s->result = np_file_serve(s->socket, s->root, s->read_only);
    return NULL;
}
struct client { struct np_file2_session *session; pthread_t thread; };
static struct client connect_server(struct server *s) {
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    s->socket = sockets[0];
    struct client client;
    pthread_attr_t attributes;
    assert(pthread_attr_init(&attributes) == 0);
    /* Exercise musl's small default even on hosts with larger defaults. */
    assert(pthread_attr_setstacksize(&attributes, 128 * 1024) == 0);
    assert(pthread_create(&client.thread, &attributes, serve, s) == 0);
    assert(pthread_attr_destroy(&attributes) == 0);
    client.session = np_file2_session_start(sockets[1], NP_FILE2_ROLE_HOST, NULL);
    assert(client.session);
    return client;
}
static void disconnect(struct client client) {
    np_file2_session_close(client.session);
    assert(pthread_join(client.thread, NULL) == 0);
}

struct reply {
    uint32_t status;
    size_t length, data_length;
    unsigned char body[65536];
    unsigned char *data;  /* NP_FILE_IO_MAX bytes */
};
static struct reply *new_reply(void) {
    struct reply *reply = calloc(1, sizeof(*reply));
    assert(reply && (reply->data = malloc(NP_FILE_IO_MAX)));
    return reply;
}
static void free_reply(struct reply *reply) { free(reply->data); free(reply); }

/* One request; `data` (if any) follows as DATA. Returns the status. */
static uint32_t call(struct client client, uint16_t opcode, const void *body, size_t length,
                     const void *data, size_t data_length, struct reply *reply) {
    struct np_file2_stream *stream = np_file2_request(client.session, opcode, body, length, !data);
    assert(stream);
    if (data) np_file2_send(stream, data, data_length, 1);
    struct np_file2_message *message = malloc(sizeof(*message)); assert(message);
    assert(np_file2_receive(stream, message) == 1 && message->type == NP_FILE2_RESPONSE);
    reply->status = message->status; reply->length = message->length;
    memcpy(reply->body, message->data, message->length);
    reply->data_length = 0;
    int result;
    while ((result = np_file2_receive(stream, message)) == 1) {
        assert(message->type == NP_FILE2_DATA && reply->data_length + message->length <= NP_FILE_IO_MAX);
        memcpy(reply->data + reply->data_length, message->data, message->length);
        reply->data_length += message->length;
    }
    assert(result == 0);
    free(message);
    np_file2_stream_release(stream);
    return reply->status;
}
static size_t put_path(unsigned char *body, const char *path) {
    size_t n = strlen(path);
    np_file_put32(body, (uint32_t)n); memcpy(body + 4, path, n);
    return 4 + n;
}
static uint32_t path_call(struct client client, uint16_t opcode, const char *path, struct reply *reply) {
    unsigned char body[4100];
    return call(client, opcode, body, put_path(body, path), NULL, 0, reply);
}
static uint32_t open_path(struct client client, unsigned flags, unsigned mode, const char *path,
                          struct reply *reply, uint64_t *handle) {
    unsigned char body[4108];
    np_file_put32(body, flags); np_file_put32(body + 4, mode);
    uint32_t status = call(client, NP_FILE_OPEN, body, 8 + put_path(body + 8, path), NULL, 0, reply);
    if (!status) { assert(reply->length >= 8 + NP_FILE_METADATA); *handle = np_file_u64(reply->body); }
    return status;
}
static uint32_t handle_call(struct client client, uint16_t opcode, uint64_t handle, uint32_t count,
                            const void *data, size_t data_length, struct reply *reply) {
    unsigned char body[12];
    np_file_put64(body, handle); np_file_put32(body + 8, count);
    return call(client, opcode, body, opcode == NP_FILE_READ ? 12 : 8, data, data_length, reply);
}
static uint32_t range(struct client client, const char *path, uint64_t offset, uint32_t count,
                      const unsigned char revision[NP_FILE_REVISION], struct reply *reply) {
    unsigned char body[12 + NP_FILE_REVISION + 4100];
    np_file_put64(body, offset); np_file_put32(body + 8, count);
    memcpy(body + 12, revision, NP_FILE_REVISION);
    return call(client, NP_FILE_RANGE, body, 12 + NP_FILE_REVISION + put_path(body + 12 + NP_FILE_REVISION, path),
                NULL, 0, reply);
}
static uint32_t staging(struct server *s, uint16_t opcode, const char *path,
                        unsigned char token[NP_FILE_STAGING_TOKEN], const char *destination) {
    struct client client = connect_server(s);
    struct reply *reply = new_reply();
    if (opcode == NP_FILE_CREATE_STAGING) assert(getentropy(token, NP_FILE_STAGING_TOKEN) == 0);
    unsigned char body[8300]; size_t size = NP_FILE_STAGING_TOKEN;
    memcpy(body, token, NP_FILE_STAGING_TOKEN);
    if (destination) size += put_path(body + size, destination);
    size += put_path(body + size, path);
    uint32_t status = call(client, opcode, body, size, NULL, 0, reply);
    assert(status || (!reply->length && !reply->data_length));
    free_reply(reply); disconnect(client);
    return status;
}

/* Counts plain directory records, checking that none is split. */
static unsigned count_names(const struct reply *reply, unsigned expected_length) {
    unsigned names = 0;
    for (size_t i = 0; i < reply->data_length;) {
        assert(i + 3 <= reply->data_length);
        unsigned n = reply->data[i + 1] | reply->data[i + 2] << 8;
        assert(n && i + 3 + n <= reply->data_length && (!expected_length || n == expected_length));
        i += 3 + n; names++;
    }
    return names;
}

static void test_staging(struct server *s) {
    const char *stage = "/.nativepipe-upload-12345678-1234-1234-1234-123456789abc";
    unsigned char token[NP_FILE_STAGING_TOKEN], invalid[NP_FILE_STAGING_TOKEN] = {0};
    s->read_only = 1;
    assert(staging(s, NP_FILE_CREATE_STAGING, stage, token, NULL) == EROFS);
    s->read_only = 0;
    assert(staging(s, NP_FILE_CREATE_STAGING, "/arbitrary", token, NULL) == EINVAL);
    assert(staging(s, NP_FILE_CREATE_STAGING, stage, token, NULL) == 0);
    assert(staging(s, NP_FILE_CREATE_STAGING, stage, invalid, NULL) == EEXIST);
    memset(invalid, 0, sizeof(invalid));
    assert(staging(s, NP_FILE_DISCARD_STAGING, stage, invalid, NULL) == EACCES);

    /* Copying an ownership marker to a different inode never gives it the
     * authority to remove the copied-to directory. */
    const char *copy = "/.nativepipe-upload-87654321-1234-1234-1234-123456789abc";
    assert(mkdirat(s->root, copy + 1, 0700) == 0);
    int original = openat(s->root, stage + 1, O_RDONLY | O_DIRECTORY); assert(original >= 0);
    int duplicate = openat(s->root, copy + 1, O_RDONLY | O_DIRECTORY); assert(duplicate >= 0);
    int original_marker = openat(original, ".nativepipe-owner", O_RDONLY); assert(original_marker >= 0);
    int duplicate_marker = openat(duplicate, ".nativepipe-owner", O_WRONLY | O_CREAT | O_EXCL, 0600); assert(duplicate_marker >= 0);
    unsigned char marker[56]; assert(read(original_marker, marker, sizeof(marker)) == sizeof(marker));
    assert(write(duplicate_marker, marker, sizeof(marker)) == sizeof(marker));
    close(original_marker); close(duplicate_marker); close(original);
    assert(staging(s, NP_FILE_DISCARD_STAGING, copy, token, NULL) == EACCES);
    assert(unlinkat(duplicate, ".nativepipe-owner", 0) == 0); close(duplicate);
    assert(unlinkat(s->root, copy + 1, AT_REMOVEDIR) == 0);
    int stage_fd = openat(s->root, stage + 1, O_RDONLY | O_DIRECTORY | O_NOFOLLOW); assert(stage_fd >= 0);
    int payload = openat(stage_fd, ".payload", O_CREAT | O_EXCL | O_WRONLY, 0640); assert(payload >= 0);
    assert(write(payload, "published", 9) == 9); close(payload);
    s->read_only = 1;
    assert(staging(s, NP_FILE_PUBLISH_STAGING, stage, token, "/target") == EROFS);
    assert(staging(s, NP_FILE_DISCARD_STAGING, stage, token, NULL) == EROFS);
    s->read_only = 0;
    assert(staging(s, NP_FILE_PUBLISH_STAGING, stage, token, "/source") == EEXIST);
    assert(staging(s, NP_FILE_PUBLISH_STAGING, stage, token, "/target") == 0);
    struct stat st;
    assert(fstatat(s->root, stage + 1, &st, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);
    payload = openat(s->root, "target", O_RDONLY); assert(payload >= 0);
    char bytes[9]; assert(read(payload, bytes, 9) == 9 && !memcmp(bytes, "published", 9)); close(payload);
    assert(unlinkat(s->root, "target", 0) == 0); close(stage_fd);

    /* Folder commit preserves the whole subtree without merging; cancellation
     * cleanup also removes legitimate nested files named like the marker. */
    assert(staging(s, NP_FILE_CREATE_STAGING, stage, token, NULL) == 0);
    stage_fd = openat(s->root, stage + 1, O_RDONLY | O_DIRECTORY | O_NOFOLLOW); assert(stage_fd >= 0);
    assert(mkdirat(stage_fd, ".payload", 0700) == 0);
    payload = openat(stage_fd, ".payload", O_RDONLY | O_DIRECTORY); assert(payload >= 0);
    int leaf = openat(payload, ".nativepipe-owner", O_CREAT | O_EXCL | O_WRONLY, 0600); assert(leaf >= 0); close(leaf);
    assert(staging(s, NP_FILE_PUBLISH_STAGING, stage, token, "/folder") == 0);
    close(payload); close(stage_fd);
    assert(staging(s, NP_FILE_CREATE_STAGING, stage, token, NULL) == 0);
    stage_fd = openat(s->root, stage + 1, O_RDONLY | O_DIRECTORY | O_NOFOLLOW); assert(stage_fd >= 0);
    assert(mkdirat(stage_fd, ".payload", 0700) == 0);
    payload = openat(stage_fd, ".payload", O_RDONLY | O_DIRECTORY); assert(payload >= 0);
    leaf = openat(payload, ".nativepipe-owner", O_CREAT | O_EXCL | O_WRONLY, 0600); assert(leaf >= 0); close(leaf);
    assert(symlinkat("/source", payload, "link") == 0);
    close(payload); close(stage_fd);
    assert(staging(s, NP_FILE_DISCARD_STAGING, stage, token, NULL) == 0);
    assert(fstatat(s->root, "source", &st, AT_SYMLINK_NOFOLLOW) == 0);
    payload = openat(s->root, "folder", O_RDONLY | O_DIRECTORY); assert(payload >= 0);
    assert(unlinkat(payload, ".nativepipe-owner", 0) == 0); close(payload);
    assert(unlinkat(s->root, "folder", AT_REMOVEDIR) == 0);

    /* A matching prefix is not ownership. Neither arbitrary directories nor
     * symlinked parents are eligible for destructive cleanup. */
    assert(mkdirat(s->root, stage + 1, 0700) == 0);
    assert(staging(s, NP_FILE_DISCARD_STAGING, stage, token, NULL) != 0);
    assert(unlinkat(s->root, stage + 1, AT_REMOVEDIR) == 0);
    assert(symlinkat(".", s->root, "staging-parent") == 0);
    assert(staging(s, NP_FILE_CREATE_STAGING,
        "/staging-parent/.nativepipe-upload-12345678-1234-1234-1234-123456789abc", token, NULL) != 0);

    struct client client = connect_server(s);
    struct reply *reply = new_reply(); uint64_t handle;
    assert(open_path(client, NP_FILE_OPEN_WRITE | NP_FILE_NOFOLLOW, 0600, "/staging-parent/blocked", reply, &handle) != 0);
    unsigned char body[64]; np_file_put32(body, NP_FILE_NOFOLLOW);
    assert(call(client, NP_FILE_MKDIR, body, 4 + put_path(body + 4, "/staging-parent/blocked"), NULL, 0, reply) != 0);
    free_reply(reply); disconnect(client);
    assert(fstatat(s->root, "blocked", &st, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);
    assert(unlinkat(s->root, "staging-parent", 0) == 0);
}

int main(void) {
    char dir[] = "/tmp/nativepipe-file-rpc.XXXXXX"; assert(mkdtemp(dir));
    int root = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC); assert(root >= 0);
#ifdef __linux__
    /* A one-shot service must wake an idle listener before joining it. Test
     * this on real Linux VMs; ordinary CI hosts may not expose AF_VSOCK. */
    struct np_file_service *service = np_file_service_start(UINT32_MAX, dir);
    if (service) {
        alarm(10);
        np_file_service_stop(service);
        for (unsigned i = 0; i < 32; i++) {
            service = np_file_service_start(UINT32_MAX, dir);
            assert(service);
            np_file_service_stop(service);
        }
        alarm(0);
        puts("file service: idle listener shutdown PASS");
    } else {
        assert(errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT || errno == EPERM || errno == ENODEV);
        puts("file service: AF_VSOCK unavailable on test host (skip)");
    }
#endif
    struct server s = {.root = root};
    struct reply *reply = new_reply();
    uint64_t handle;

    /* A file larger than one request moves through one handle in order. */
    unsigned char *block = malloc(NP_FILE_IO_MAX); assert(block);
    for (unsigned i = 0; i < NP_FILE_IO_MAX; i++) block[i] = (unsigned char)(i * 37);
    struct client client = connect_server(&s);
    assert(open_path(client, NP_FILE_OPEN_WRITE, 0640, "/uploaded", reply, &handle) == 0);
    assert(S_ISREG(np_file_u32(reply->body + 8)));
    for (unsigned i = 0; i < 10; i++) {
        assert(handle_call(client, NP_FILE_WRITE, handle, 0, block, NP_FILE_IO_MAX, reply) == 0);
        assert(reply->length == 4 && np_file_u32(reply->body) == NP_FILE_IO_MAX);
    }
    assert(handle_call(client, NP_FILE_CLOSE, handle, 0, NULL, 0, reply) == 0);
    /* A closed handle is gone. */
    assert(handle_call(client, NP_FILE_CLOSE, handle, 0, NULL, 0, reply) == EBADF);
    struct stat st; assert(fstatat(root, "uploaded", &st, 0) == 0);
    assert(st.st_size == 10 * NP_FILE_IO_MAX && (st.st_mode & 0777) == 0640);
    assert(open_path(client, 0, 0, "/uploaded", reply, &handle) == 0);
    assert(np_file_u64(reply->body + 8 + 12) == 10 * NP_FILE_IO_MAX);
    assert(handle_call(client, NP_FILE_WRITE, handle, 0, "x", 1, reply) == EBADF);
    unsigned blocks = 0;
    for (;;) {
        assert(handle_call(client, NP_FILE_READ, handle, NP_FILE_IO_MAX, NULL, 0, reply) == 0);
        if (!reply->data_length) break;
        assert(reply->data_length == NP_FILE_IO_MAX && !memcmp(reply->data, block, NP_FILE_IO_MAX));
        blocks++;
    }
    assert(blocks == 10);
    assert(handle_call(client, NP_FILE_READ, handle, NP_FILE_IO_MAX + 1, NULL, 0, reply) == EINVAL);
    assert(handle_call(client, NP_FILE_READ, handle, 0, NULL, 0, reply) == EINVAL);
    assert(handle_call(client, NP_FILE_READ, handle + 100, 1, NULL, 0, reply) == EBADF);
    /* Handles are bounded per session and released with it. */
    unsigned opened = 1;
    while (open_path(client, 0, 0, "/uploaded", reply, &handle) == 0) opened++;
    assert(reply->status == EMFILE && opened == NP_FILE_HANDLES);
    disconnect(client);
    client = connect_server(&s);
    assert(open_path(client, 0, 0, "/uploaded", reply, &handle) == 0);
    disconnect(client);
    free(block);

    /* Reads transfer exactly a requested sparse-file range, never its prefix
     * or the entire multi-terabyte apparent size. The revision pins identity. */
    int sparse = openat(root, "sparse", O_CREAT | O_EXCL | O_RDWR, 0751); assert(sparse >= 0);
    uint64_t large_offset = UINT64_C(1) << 40;
    assert(pwrite(sparse, "range-bytes", 11, (off_t)large_offset) == 11);
    client = connect_server(&s);
    assert(path_call(client, NP_FILE_SNAPSHOT, "/sparse", reply) == 0);
    assert(reply->length == NP_FILE_METADATA + NP_FILE_REVISION && np_file_u64(reply->body + 12) == large_offset + 11);
    unsigned char version[NP_FILE_REVISION]; memcpy(version, reply->body + NP_FILE_METADATA, sizeof(version));
    assert(range(client, "/sparse", large_offset + 2, 5, version, reply) == 0);
    assert(reply->data_length == 5 && !memcmp(reply->data, "nge-b", 5));
    assert(range(client, "/sparse", large_offset + 9, 8, version, reply) == 0);
    assert(reply->data_length == 2 && !memcmp(reply->data, "es", 2));
    for (unsigned i = 0; i < 2; i++) {
        assert(range(client, "/sparse", large_offset + 11, i ? 0 : 10, version, reply) == 0 && !reply->data_length);
    }
    assert(range(client, "/sparse", UINT64_MAX, 1, version, reply) == EINVAL);
    assert(range(client, "/sparse", 0, NP_FILE_IO_MAX + 1, version, reply) == EINVAL);
    assert(symlinkat("sparse", root, "sparse-link") == 0);
    assert(range(client, "/sparse-link", large_offset, 5, version, reply) != 0);
    assert(symlinkat(".", root, "folder-link") == 0);
    assert(range(client, "/folder-link/sparse", large_offset, 5, version, reply) != 0);
    assert(open_path(client, NP_FILE_OPEN_DIRECTORY | NP_FILE_NOFOLLOW, 0, "/folder-link", reply, &handle) != 0);
    assert(pwrite(sparse, "changed", 7, (off_t)large_offset) == 7);
    assert(range(client, "/sparse", large_offset, 7, version, reply) == NP_FILE_STALE && !reply->data_length);
    disconnect(client);
    assert(unlinkat(root, "sparse-link", 0) == 0); assert(unlinkat(root, "folder-link", 0) == 0);
    close(sparse); assert(unlinkat(root, "sparse", 0) == 0);

    /* No silent overwrite, special-file blocking or read-only bypass. */
    client = connect_server(&s);
    assert(open_path(client, NP_FILE_OPEN_WRITE, 0600, "/uploaded", reply, &handle) == EEXIST);
    assert(mkfifoat(root, "fifo", 0600) == 0);
    assert(open_path(client, 0, 0, "/fifo", reply, &handle) == EINVAL);
    assert(open_path(client, NP_FILE_REPLACE, 0, "/uploaded", reply, &handle) == EINVAL);
    assert(open_path(client, NP_FILE_OPEN_WRITE | NP_FILE_OPEN_DIRECTORY, 0600, "/new", reply, &handle) == EINVAL);
    assert(path_call(client, 99, "/uploaded", reply) == ENOSYS);
    disconnect(client);
    s.read_only = 1; client = connect_server(&s);
    assert(open_path(client, NP_FILE_OPEN_WRITE, 0600, "/denied", reply, &handle) == EROFS);
    disconnect(client); s.read_only = 0;

    /* Replacing truncates; an empty file reads as an immediate end. */
    int source = openat(root, "source", O_CREAT | O_EXCL | O_RDWR, 0600); assert(source >= 0);
    assert(write(source, "old contents", 12) == 12);
    client = connect_server(&s);
    assert(open_path(client, NP_FILE_OPEN_WRITE | NP_FILE_REPLACE, 0600, "/source", reply, &handle) == 0);
    assert(np_file_u64(reply->body + 8 + 12) == 0);
    assert(handle_call(client, NP_FILE_CLOSE, handle, 0, NULL, 0, reply) == 0);
    assert(open_path(client, 0, 0, "/source", reply, &handle) == 0);
    assert(handle_call(client, NP_FILE_READ, handle, 4096, NULL, 0, reply) == 0 && !reply->data_length);
    assert(path_call(client, NP_FILE_STAT, "/source", reply) == 0 && reply->length == NP_FILE_METADATA);
    assert(open_path(client, 0, 0, "/", reply, &handle) == 0 && S_ISDIR(np_file_u32(reply->body + 8)));
    assert(handle_call(client, NP_FILE_READ, handle, NP_FILE_IO_MAX, NULL, 0, reply) == 0);
    assert(count_names(reply, 0) == 3);
    assert(handle_call(client, NP_FILE_READ, handle, NP_FILE_IO_MAX, NULL, 0, reply) == 0 && !reply->data_length);
    assert(open_path(client, NP_FILE_OPEN_DIRECTORY, 0, "/source", reply, &handle) == ENOTDIR);
    disconnect(client);

    /* A malformed frame ends the session, and the server returns. */
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    s.socket = sockets[0];
    pthread_t thread; assert(pthread_create(&thread, NULL, serve, &s) == 0);
    unsigned char malformed[16] = {'N','P','F','R',1,3};
    assert(write(sockets[1], malformed, sizeof(malformed)) == sizeof(malformed));
    assert(pthread_join(thread, NULL) == 0 && s.result == 0);
    close(sockets[1]);

    /* The actual user home and all lstat fields arrive with the listing;
     * links and special files appear without opening their targets. */
    char *saved_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    assert(setenv("HOME", "/", 1) == 0);
    assert(symlinkat("source", root, "browse-link") == 0);
    const unsigned browse = NP_FILE_OPEN_DIRECTORY | NP_FILE_OPEN_STAT | NP_FILE_OPEN_HOME | NP_FILE_NOFOLLOW;
    client = connect_server(&s);
    assert(open_path(client, browse, 0, "", reply, &handle) == 0);
    size_t base = 8 + NP_FILE_METADATA;
    assert(reply->length == base + 10 && np_file_u32(reply->body + base) == 1 && np_file_u32(reply->body + base + 4) == 1);
    assert(reply->body[base + 8] == '/' && reply->body[base + 9] == '/');
    assert(handle_call(client, NP_FILE_READ, handle, NP_FILE_IO_MAX, NULL, 0, reply) == 0);
    unsigned seen = 0;
    for (size_t i = 0; i < reply->data_length;) {
        assert(i + 30 <= reply->data_length);
        unsigned n = reply->data[i] | reply->data[i + 1] << 8;
        assert(n && n <= 255 && i + 30 + n <= reply->data_length);
        char entry[256]; memcpy(entry, reply->data + i + 30, n); entry[n] = 0;
        struct stat info; assert(fstatat(root, entry, &info, AT_SYMLINK_NOFOLLOW) == 0);
        assert(np_file_u32(reply->data + i + 2) == (uint32_t)info.st_mode);
        assert(np_file_u64(reply->data + i + 14) == (uint64_t)info.st_size);
        assert(np_file_u64(reply->data + i + 22) == (uint64_t)info.st_mtime);
        seen++; i += 30 + n;
    }
    assert(seen == 4);
    assert(handle_call(client, NP_FILE_READ, handle, NP_FILE_IO_MAX, NULL, 0, reply) == 0 && !reply->data_length);
    if (saved_home) { assert(setenv("HOME", saved_home, 1) == 0); free(saved_home); }
    else assert(unsetenv("HOME") == 0);
    assert(open_path(client, browse, 0, "/browse-link", reply, &handle) != 0);
    assert(open_path(client, NP_FILE_OPEN_DIRECTORY, 0, "", reply, &handle) == EINVAL);
    disconnect(client);
    assert(unlinkat(root, "browse-link", 0) == 0);
    test_staging(&s);

    /* Directory pages never split a name, and a page too small for the next
     * name is refused rather than skipping it. */
    assert(mkdirat(root, "many", 0700) == 0);
    int many = openat(root, "many", O_RDONLY | O_DIRECTORY); assert(many >= 0);
    char name[220];
    for (unsigned i = 0; i < 600; i++) {
        memset(name, 'x', 200); snprintf(name + 200, sizeof(name) - 200, "%04u", i);
        int file = openat(many, name, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(file >= 0); close(file);
    }
    client = connect_server(&s);
    assert(open_path(client, 0, 0, "/many", reply, &handle) == 0);
    assert(handle_call(client, NP_FILE_READ, handle, 100, NULL, 0, reply) == EMSGSIZE);
    unsigned entries = 0, pages = 0;
    for (;;) {
        assert(handle_call(client, NP_FILE_READ, handle, 65536, NULL, 0, reply) == 0);
        if (!reply->data_length) break;
        entries += count_names(reply, 204); pages++;
    }
    assert(entries == 600 && pages > 1);
    assert(open_path(client, NP_FILE_OPEN_DIRECTORY | NP_FILE_OPEN_STAT | NP_FILE_NOFOLLOW, 0, "/many", reply, &handle) == 0);
    entries = 0; pages = 0;
    for (;;) {
        assert(handle_call(client, NP_FILE_READ, handle, 65536, NULL, 0, reply) == 0);
        if (!reply->data_length) break;
        for (size_t i = 0; i < reply->data_length;) {
            unsigned n = reply->data[i] | reply->data[i + 1] << 8;
            assert(n == 204 && i + 30 + n <= reply->data_length);
            assert(S_ISREG(np_file_u32(reply->data + i + 2)) && !np_file_u64(reply->data + i + 14));
            entries++; i += 30 + n;
        }
        pages++;
    }
    assert(entries == 600 && pages > 1);
    disconnect(client);
    for (unsigned i = 0; i < 600; i++) {
        memset(name, 'x', 200); snprintf(name + 200, sizeof(name) - 200, "%04u", i);
        assert(unlinkat(many, name, 0) == 0);
    }
    close(many); assert(unlinkat(root, "many", AT_REMOVEDIR) == 0);

    if (geteuid() != 0) {
        assert(fchmod(source, 0) == 0);
        client = connect_server(&s);
        assert(open_path(client, 0, 0, "/source", reply, &handle) == EACCES);
        disconnect(client);
        assert(fchmod(source, 0600) == 0);
    }
#ifdef __linux__
    /* Absolute symlinks resolve within the supplied root, never host /. */
    assert(symlinkat("/uploaded", root, "link") == 0);
    int inside = np_file_open(root, "/link", O_RDONLY, 0); assert(inside >= 0); close(inside);
    assert(unlinkat(root, "link", 0) == 0);
    assert(symlinkat("/etc/passwd", root, "outside") == 0);
    assert(np_file_open(root, "/outside", O_RDONLY, 0) < 0);
    assert(unlinkat(root, "outside", 0) == 0);
    /* procfs often reports st_size=0 while its contents are nonempty. */
    int system_root = open("/", O_RDONLY | O_DIRECTORY); assert(system_root >= 0);
    s.root = system_root; client = connect_server(&s);
    assert(open_path(client, 0, 0, "/proc/meminfo", reply, &handle) == 0);
    assert(handle_call(client, NP_FILE_READ, handle, NP_FILE_IO_MAX, NULL, 0, reply) == 0 && reply->data_length > 0);
    disconnect(client); close(system_root); s.root = root;
#endif
    free_reply(reply);
    close(source); assert(unlinkat(root, "source", 0) == 0);
    assert(unlinkat(root, "uploaded", 0) == 0); assert(unlinkat(root, "fifo", 0) == 0);
    close(root); assert(rmdir(dir) == 0);
    puts("file service: transfer, ranges, handles, listings and file safety PASS");
}
