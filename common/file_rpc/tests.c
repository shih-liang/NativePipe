#define _GNU_SOURCE
#undef NDEBUG
#include "np_file_rpc.h"
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
    close(s->socket);
    return NULL;
}
static void request(int fd, int type, const char *path, int replace) {
    unsigned char body[4096 + 8]; size_t n = strlen(path);
    np_file_put32(body, (uint32_t)n); memcpy(body + 4, path, n);
    if (type == NP_FILE_WRITE) np_file_put32(body + 4 + n, 0600);
    assert(np_file_send(fd, type, replace, 0, body, n + 4 + (type == NP_FILE_WRITE ? 4 : 0)) == 0);
}
static void range_request(int fd, const char *path, uint64_t offset, uint32_t count,
                          const unsigned char revision[NP_FILE_REVISION]) {
    unsigned char body[4096 + 4 + 12 + NP_FILE_REVISION]; size_t n = strlen(path);
    np_file_put32(body, (uint32_t)n); memcpy(body + 4, path, n);
    np_file_put64(body + 4 + n, offset); np_file_put32(body + 12 + n, count);
    memcpy(body + 16 + n, revision, NP_FILE_REVISION);
    assert(np_file_send(fd, NP_FILE_RANGE, 0, 0, body, n + 16 + NP_FILE_REVISION) == 0);
}
static int start(struct server *s, pthread_t *thread) {
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    s->socket = sockets[0];
    pthread_attr_t attributes;
    assert(pthread_attr_init(&attributes) == 0);
    /* Exercise musl's small default even on hosts with larger defaults. */
    assert(pthread_attr_setstacksize(&attributes, 128 * 1024) == 0);
    assert(pthread_create(thread, &attributes, serve, s) == 0);
    assert(pthread_attr_destroy(&attributes) == 0);
    return sockets[1];
}
static void finish(int fd, pthread_t thread) {
    shutdown(fd, SHUT_RDWR); close(fd); assert(pthread_join(thread, NULL) == 0);
}

static void staging_request(int fd, int type, const char *path,
                            const unsigned char token[NP_FILE_STAGING_TOKEN], const char *destination) {
    unsigned char body[8192 + NP_FILE_STAGING_TOKEN]; size_t n = strlen(path), size = 4 + n;
    np_file_put32(body, (uint32_t)n); memcpy(body + 4, path, n);
    if (token) { memcpy(body + size, token, NP_FILE_STAGING_TOKEN); size += NP_FILE_STAGING_TOKEN; }
    if (destination) {
        size_t target = strlen(destination);
        np_file_put32(body + size, (uint32_t)target); memcpy(body + size + 4, destination, target); size += 4 + target;
    }
    assert(np_file_send(fd, type, 0, 0, body, size) == 0);
}
static uint32_t staging_operation(struct server *s, int type, const char *path,
                                 unsigned char token[NP_FILE_STAGING_TOKEN], const char *destination) {
    pthread_t thread; int fd = start(s, &thread);
    struct np_file_frame *frame = malloc(sizeof(*frame)); assert(frame);
    if (type == NP_FILE_CREATE_STAGING) assert(getentropy(token, NP_FILE_STAGING_TOKEN) == 0);
    staging_request(fd, type, path, token, destination);
    assert(np_file_receive(fd, frame) == 0);
    uint32_t status = frame->status;
    if (!status) {
        if (type == NP_FILE_CREATE_STAGING) {
            assert(frame->type == NP_FILE_METADATA && frame->length == NP_FILE_STAGING_TOKEN);
            memcpy(token, frame->data, NP_FILE_STAGING_TOKEN);
        } else assert(frame->type == NP_FILE_END && !frame->length);
    }
    finish(fd, thread); free(frame); return status;
}
static void test_staging(struct server *s) {
    const char *stage = "/.nativepipe-upload-12345678-1234-1234-1234-123456789abc";
    unsigned char token[NP_FILE_STAGING_TOKEN], invalid[NP_FILE_STAGING_TOKEN] = {0};
    s->read_only = 1;
    assert(staging_operation(s, NP_FILE_CREATE_STAGING, stage, token, NULL) == EROFS);
    s->read_only = 0;
    assert(staging_operation(s, NP_FILE_CREATE_STAGING, "/arbitrary", token, NULL) == EINVAL);
    assert(staging_operation(s, NP_FILE_CREATE_STAGING, stage, token, NULL) == 0);
    assert(staging_operation(s, NP_FILE_CREATE_STAGING, stage, invalid, NULL) == EEXIST);
    assert(staging_operation(s, NP_FILE_DISCARD_STAGING, stage, invalid, NULL) == EACCES);

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
    assert(staging_operation(s, NP_FILE_DISCARD_STAGING, copy, token, NULL) == EACCES);
    assert(unlinkat(duplicate, ".nativepipe-owner", 0) == 0); close(duplicate);
    assert(unlinkat(s->root, copy + 1, AT_REMOVEDIR) == 0);
    int staging = openat(s->root, stage + 1, O_RDONLY | O_DIRECTORY | O_NOFOLLOW); assert(staging >= 0);
    int payload = openat(staging, ".payload", O_CREAT | O_EXCL | O_WRONLY, 0640); assert(payload >= 0);
    assert(write(payload, "published", 9) == 9); close(payload);
    s->read_only = 1;
    assert(staging_operation(s, NP_FILE_PUBLISH_STAGING, stage, token, "/target") == EROFS);
    assert(staging_operation(s, NP_FILE_DISCARD_STAGING, stage, token, NULL) == EROFS);
    s->read_only = 0;
    assert(staging_operation(s, NP_FILE_PUBLISH_STAGING, stage, token, "/source") == EEXIST);
    assert(staging_operation(s, NP_FILE_PUBLISH_STAGING, stage, token, "/target") == 0);
    struct stat st;
    assert(fstatat(s->root, stage + 1, &st, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);
    payload = openat(s->root, "target", O_RDONLY); assert(payload >= 0);
    char bytes[9]; assert(read(payload, bytes, 9) == 9 && !memcmp(bytes, "published", 9)); close(payload);
    assert(unlinkat(s->root, "target", 0) == 0); close(staging);

    /* Folder commit preserves the whole subtree without merging; cancellation
     * cleanup also removes legitimate nested files named like the marker. */
    assert(staging_operation(s, NP_FILE_CREATE_STAGING, stage, token, NULL) == 0);
    staging = openat(s->root, stage + 1, O_RDONLY | O_DIRECTORY | O_NOFOLLOW); assert(staging >= 0);
    assert(mkdirat(staging, ".payload", 0700) == 0);
    payload = openat(staging, ".payload", O_RDONLY | O_DIRECTORY); assert(payload >= 0);
    int leaf = openat(payload, ".nativepipe-owner", O_CREAT | O_EXCL | O_WRONLY, 0600); assert(leaf >= 0); close(leaf);
    assert(staging_operation(s, NP_FILE_PUBLISH_STAGING, stage, token, "/folder") == 0);
    close(payload); close(staging);
    assert(staging_operation(s, NP_FILE_CREATE_STAGING, stage, token, NULL) == 0);
    staging = openat(s->root, stage + 1, O_RDONLY | O_DIRECTORY | O_NOFOLLOW); assert(staging >= 0);
    assert(mkdirat(staging, ".payload", 0700) == 0);
    payload = openat(staging, ".payload", O_RDONLY | O_DIRECTORY); assert(payload >= 0);
    leaf = openat(payload, ".nativepipe-owner", O_CREAT | O_EXCL | O_WRONLY, 0600); assert(leaf >= 0); close(leaf);
    assert(symlinkat("/source", payload, "link") == 0);
    close(payload); close(staging);
    assert(staging_operation(s, NP_FILE_DISCARD_STAGING, stage, token, NULL) == 0);
    assert(fstatat(s->root, "source", &st, AT_SYMLINK_NOFOLLOW) == 0);
    payload = openat(s->root, "folder", O_RDONLY | O_DIRECTORY); assert(payload >= 0);
    assert(unlinkat(payload, ".nativepipe-owner", 0) == 0); close(payload);
    assert(unlinkat(s->root, "folder", AT_REMOVEDIR) == 0);

    /* A matching prefix is not ownership. Neither arbitrary directories nor
     * symlinked parents are eligible for destructive cleanup. */
    assert(mkdirat(s->root, stage + 1, 0700) == 0);
    assert(staging_operation(s, NP_FILE_DISCARD_STAGING, stage, token, NULL) != 0);
    assert(unlinkat(s->root, stage + 1, AT_REMOVEDIR) == 0);
    assert(symlinkat(".", s->root, "staging-parent") == 0);
    assert(staging_operation(s, NP_FILE_CREATE_STAGING,
        "/staging-parent/.nativepipe-upload-12345678-1234-1234-1234-123456789abc", token, NULL) != 0);

    pthread_t thread; int fd = start(s, &thread);
    struct np_file_frame *frame = malloc(sizeof(*frame)); assert(frame);
    request(fd, NP_FILE_WRITE, "/staging-parent/blocked", NP_FILE_NOFOLLOW);
    assert(np_file_receive(fd, frame) == 0 && frame->status); finish(fd, thread);
    fd = start(s, &thread); request(fd, NP_FILE_MKDIR, "/staging-parent/blocked", NP_FILE_NOFOLLOW);
    assert(np_file_receive(fd, frame) == 0 && frame->status); finish(fd, thread); free(frame);
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
    struct server s = {.root = root}; pthread_t thread;
    struct np_file_frame frame;
    /* More than the old 7 MiB control-message limit; no whole-file allocation. */
    int source = openat(root, "source", O_CREAT | O_EXCL | O_RDWR, 0600); assert(source >= 0);
    unsigned char block[NP_FILE_CHUNK];
    for (unsigned i = 0; i < sizeof(block); i++) block[i] = (unsigned char)(i * 37);
    for (unsigned i = 0; i < 160; i++) assert(write(source, block, sizeof(block)) == sizeof(block));
    assert(lseek(source, 0, SEEK_SET) == 0);
    int fd = start(&s, &thread); request(fd, NP_FILE_WRITE, "/uploaded", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_METADATA);
    assert(np_file_send_stream(fd, source, UINT64_MAX) == 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_END && frame.status == 0);
    assert(np_file_u64(frame.data) == 160 * sizeof(block)); finish(fd, thread);
    fd = start(&s, &thread); request(fd, NP_FILE_READ, "/uploaded", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_METADATA);
    unsigned blocks = 0;
    for (;;) {
        assert(np_file_receive(fd, &frame) == 0 && !frame.status);
        if (frame.type == NP_FILE_END) break;
        assert(frame.type == NP_FILE_DATA && frame.length == sizeof(block));
        assert(!memcmp(frame.data, block, sizeof(block))); blocks++;
    }
    assert(blocks == 160); finish(fd, thread);
    /* Reads transfer exactly a requested sparse-file range, never its prefix
     * or the entire multi-terabyte apparent size. The revision pins identity. */
    int sparse = openat(root, "sparse", O_CREAT | O_EXCL | O_RDWR, 0751); assert(sparse >= 0);
    uint64_t large_offset = UINT64_C(1) << 40;
    assert(pwrite(sparse, "range-bytes", 11, (off_t)large_offset) == 11);
    fd = start(&s, &thread); request(fd, NP_FILE_SNAPSHOT, "/sparse", 0);
    assert(np_file_receive(fd, &frame) == 0 && !frame.status && frame.length == 28 + NP_FILE_REVISION);
    assert(np_file_u64(frame.data + 12) == large_offset + 11);
    unsigned char version[NP_FILE_REVISION]; memcpy(version, frame.data + 28, sizeof(version)); finish(fd, thread);
    fd = start(&s, &thread); range_request(fd, "/sparse", large_offset + 2, 5, version);
    assert(np_file_receive(fd, &frame) == 0 && !frame.status && frame.type == NP_FILE_DATA && frame.length == 5);
    assert(!memcmp(frame.data, "nge-b", 5));
    assert(np_file_receive(fd, &frame) == 0 && !frame.status && frame.type == NP_FILE_END && np_file_u64(frame.data) == 5);
    finish(fd, thread);
    fd = start(&s, &thread); range_request(fd, "/sparse", large_offset + 9, 8, version);
    assert(np_file_receive(fd, &frame) == 0 && !frame.status && frame.type == NP_FILE_DATA && frame.length == 2);
    assert(!memcmp(frame.data, "es", 2));
    assert(np_file_receive(fd, &frame) == 0 && !frame.status && np_file_u64(frame.data) == 2); finish(fd, thread);
    for (unsigned i = 0; i < 2; i++) {
        fd = start(&s, &thread); range_request(fd, "/sparse", large_offset + 11, i ? 0 : 10, version);
        assert(np_file_receive(fd, &frame) == 0 && !frame.status && frame.type == NP_FILE_END && !np_file_u64(frame.data));
        finish(fd, thread);
    }
    fd = start(&s, &thread); range_request(fd, "/sparse", UINT64_MAX, 1, version);
    assert(np_file_receive(fd, &frame) == 0 && frame.status == EINVAL); finish(fd, thread);
    fd = start(&s, &thread); range_request(fd, "/sparse", 0, NP_FILE_RANGE_MAX + 1, version);
    assert(np_file_receive(fd, &frame) == 0 && frame.status == EINVAL); finish(fd, thread);
    assert(symlinkat("sparse", root, "sparse-link") == 0);
    fd = start(&s, &thread); range_request(fd, "/sparse-link", large_offset, 5, version);
    assert(np_file_receive(fd, &frame) == 0 && frame.status); finish(fd, thread);
    assert(symlinkat(".", root, "folder-link") == 0);
    fd = start(&s, &thread); range_request(fd, "/folder-link/sparse", large_offset, 5, version);
    assert(np_file_receive(fd, &frame) == 0 && frame.status); finish(fd, thread);
    fd = start(&s, &thread); request(fd, NP_FILE_DIRECTORY, "/folder-link", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.status); finish(fd, thread);
    assert(pwrite(sparse, "changed", 7, (off_t)large_offset) == 7);
    fd = start(&s, &thread); range_request(fd, "/sparse", large_offset, 7, version);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_END && frame.status == NP_FILE_STALE); finish(fd, thread);
    assert(unlinkat(root, "sparse-link", 0) == 0); assert(unlinkat(root, "folder-link", 0) == 0);
    close(sparse); assert(unlinkat(root, "sparse", 0) == 0);
    /* No silent overwrite, special-file blocking or read-only bypass. */
    fd = start(&s, &thread); request(fd, NP_FILE_WRITE, "/uploaded", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.status == EEXIST); finish(fd, thread);
    assert(mkfifoat(root, "fifo", 0600) == 0);
    fd = start(&s, &thread); request(fd, NP_FILE_READ, "/fifo", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.status == EINVAL); finish(fd, thread);
    s.read_only = 1; fd = start(&s, &thread); request(fd, NP_FILE_WRITE, "/denied", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.status == EROFS); finish(fd, thread);
    s.read_only = 0;
    /* Cancelling while the sender is backpressured must wake it. */
    fd = start(&s, &thread); request(fd, NP_FILE_READ, "/uploaded", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_METADATA);
    finish(fd, thread); assert(s.result < 0);
    /* Empty files still get a complete END, and listings preserve names. */
    assert(ftruncate(source, 0) == 0);
    fd = start(&s, &thread); request(fd, NP_FILE_READ, "/source", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_METADATA);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_END && !np_file_u64(frame.data));
    finish(fd, thread);
    fd = start(&s, &thread); request(fd, NP_FILE_LIST, "/", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_METADATA);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_ENTRIES);
    unsigned names = 0;
    for (size_t i = 0; i < frame.length;) {
        assert(i + 3 <= frame.length); unsigned n = frame.data[i + 1] | frame.data[i + 2] << 8;
        i += 3 + n; assert(i <= frame.length); names++;
    }
    assert(names == 3);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_END); finish(fd, thread);
    fd = start(&s, &thread);
    assert(!np_file_peer_is_host(fd));
    unsigned char malformed[16] = {'N','P','F','R',1,NP_FILE_READ};
    np_file_put32(malformed + 8, NP_FILE_CHUNK + 1);
    assert(write(fd, malformed, sizeof(malformed)) == sizeof(malformed)); finish(fd, thread);
    assert(s.result < 0);

    /* The actual user home and all lstat fields arrive in one bounded stream;
     * links and special files appear without opening their targets. */
    char *saved_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    assert(setenv("HOME", "/", 1) == 0);
    assert(symlinkat("source", root, "browse-link") == 0);
    fd = start(&s, &thread); request(fd, NP_FILE_BROWSE, "", 0);
    assert(np_file_receive(fd, &frame) == 0 && !frame.status && frame.type == NP_FILE_METADATA);
    assert(frame.length == 10 && np_file_u32(frame.data) == 1 && np_file_u32(frame.data + 4) == 1);
    assert(frame.data[8] == '/' && frame.data[9] == '/');
    unsigned seen = 0;
    while (np_file_receive(fd, &frame) == 0 && frame.type != NP_FILE_END) {
        assert(frame.type == NP_FILE_ENTRIES && !frame.status && frame.length <= NP_FILE_CHUNK);
        for (size_t i = 0; i < frame.length;) {
            assert(i + 30 <= frame.length);
            unsigned n = frame.data[i] | frame.data[i + 1] << 8;
            assert(n && n <= 255 && i + 30 + n <= frame.length);
            char entry[256]; memcpy(entry, frame.data + i + 30, n); entry[n] = 0;
            struct stat info; assert(fstatat(root, entry, &info, AT_SYMLINK_NOFOLLOW) == 0);
            assert(np_file_u32(frame.data + i + 2) == (uint32_t)info.st_mode);
            assert(np_file_u64(frame.data + i + 14) == (uint64_t)info.st_size);
            assert(np_file_u64(frame.data + i + 22) == (uint64_t)info.st_mtime);
            seen++; i += 30 + n;
        }
    }
    assert(frame.type == NP_FILE_END && !frame.status && frame.length == 8 && np_file_u64(frame.data) == seen && seen == 4);
    finish(fd, thread);
    if (saved_home) { assert(setenv("HOME", saved_home, 1) == 0); free(saved_home); }
    else assert(unsetenv("HOME") == 0);
    fd = start(&s, &thread); request(fd, NP_FILE_BROWSE, "/browse-link", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.status); finish(fd, thread);
    assert(unlinkat(root, "browse-link", 0) == 0);
    test_staging(&s);

    /* Directory batches must cross a frame boundary without splitting names. */
    assert(mkdirat(root, "many", 0700) == 0);
    int many = openat(root, "many", O_RDONLY | O_DIRECTORY); assert(many >= 0);
    char name[220];
    for (unsigned i = 0; i < 600; i++) {
        memset(name, 'x', 200); snprintf(name + 200, sizeof(name) - 200, "%04u", i);
        int file = openat(many, name, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(file >= 0); close(file);
    }
    fd = start(&s, &thread); request(fd, NP_FILE_LIST, "/many", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_METADATA);
    unsigned entries = 0, batches = 0;
    while (np_file_receive(fd, &frame) == 0 && frame.type != NP_FILE_END) {
        assert(frame.type == NP_FILE_ENTRIES && !frame.status); batches++;
        for (size_t i = 0; i < frame.length;) {
            assert(i + 3 <= frame.length);
            unsigned n = frame.data[i + 1] | frame.data[i + 2] << 8;
            assert(n == 204 && i + 3 + n <= frame.length); entries++; i += 3 + n;
        }
    }
    assert(frame.type == NP_FILE_END && !frame.status && entries == 600 && batches > 1); finish(fd, thread);

    fd = start(&s, &thread); request(fd, NP_FILE_BROWSE, "/many", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_METADATA && !frame.status);
    entries = 0; batches = 0;
    while (np_file_receive(fd, &frame) == 0 && frame.type != NP_FILE_END) {
        assert(frame.type == NP_FILE_ENTRIES && !frame.status && frame.length <= NP_FILE_CHUNK); batches++;
        for (size_t i = 0; i < frame.length;) {
            assert(i + 30 <= frame.length);
            unsigned n = frame.data[i] | frame.data[i + 1] << 8;
            assert(n == 204 && i + 30 + n <= frame.length);
            assert(S_ISREG(np_file_u32(frame.data + i + 2)) && !np_file_u64(frame.data + i + 14));
            entries++; i += 30 + n;
        }
    }
    assert(frame.type == NP_FILE_END && !frame.status && np_file_u64(frame.data) == 600 && entries == 600 && batches > 1);
    finish(fd, thread);

    for (unsigned i = 0; i < 600; i++) {
        memset(name, 'x', 200); snprintf(name + 200, sizeof(name) - 200, "%04u", i);
        assert(unlinkat(many, name, 0) == 0);
    }
    close(many); assert(unlinkat(root, "many", AT_REMOVEDIR) == 0);
    if (geteuid() != 0) {
        assert(fchmod(source, 0) == 0);
        fd = start(&s, &thread); request(fd, NP_FILE_READ, "/source", 0);
        assert(np_file_receive(fd, &frame) == 0 && frame.status == EACCES); finish(fd, thread);
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
    /* procfs often reports st_size=0 while its read stream is nonempty. */
    int system_root = open("/", O_RDONLY | O_DIRECTORY); assert(system_root >= 0);
    s.root = system_root; fd = start(&s, &thread); request(fd, NP_FILE_READ, "/proc/meminfo", 0);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_METADATA);
    assert(np_file_receive(fd, &frame) == 0 && frame.type == NP_FILE_DATA && frame.length > 0);
    finish(fd, thread); close(system_root); s.root = root;
#endif
    /* A source read failure must fail locally as well as at the receiver. */
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(np_file_send_stream(pair[0], -1, UINT64_MAX) < 0 && errno == EBADF);
    assert(np_file_receive(pair[1], &frame) == 0);
    assert(frame.type == NP_FILE_END && frame.status == EBADF);
    close(pair[0]); close(pair[1]);
    close(source); assert(unlinkat(root, "source", 0) == 0);
    assert(unlinkat(root, "uploaded", 0) == 0); assert(unlinkat(root, "fifo", 0) == 0);
    close(root); assert(rmdir(dir) == 0);
    puts("file RPC: large transfer, content, EOF, cancellation, listing and file safety PASS");
}
