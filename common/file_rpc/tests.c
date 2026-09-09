#define _GNU_SOURCE
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
static int start(struct server *s, pthread_t *thread) {
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    s->socket = sockets[0];
    assert(pthread_create(thread, NULL, serve, s) == 0);
    return sockets[1];
}
static void finish(int fd, pthread_t thread) {
    shutdown(fd, SHUT_RDWR); close(fd); assert(pthread_join(thread, NULL) == 0);
}
int main(void) {
    char dir[] = "/tmp/nativepipe-file-rpc.XXXXXX"; assert(mkdtemp(dir));
    int root = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC); assert(root >= 0);
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
