#define _GNU_SOURCE
#undef NDEBUG
#include "np_drop_fs.h"
#include "np_file_rpc.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* FUSE request/reply layouts used by the test (Linux ABI, as in np_drop_fs.c). */
struct in_header { uint32_t len, opcode; uint64_t unique, nodeid; uint32_t uid, gid, pid; uint16_t extlen, pad; };
struct out_header { uint32_t len; int32_t error; uint64_t unique; };
struct attr {
    uint64_t ino, size, blocks, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec, mode, nlink, uid, gid, rdev, blksize, flags;
};
struct entry_out { uint64_t nodeid, generation, entry_valid, attr_valid; uint32_t ev, av; struct attr attr; };
struct attr_out { uint64_t valid; uint32_t nsec, dummy; struct attr attr; };
struct open_out { uint64_t fh; uint32_t flags; int32_t backing; };
struct read_in { uint64_t fh, offset; uint32_t size, read_flags; uint64_t owner; uint32_t flags, pad; };
struct dirent_head { uint64_t ino, off; uint32_t namelen, type; };
enum { LOOKUP = 1, FORGET = 2, GETATTR = 3, SETATTR = 4, MKDIR = 9, UNLINK = 10, RENAME = 12, OPEN = 14,
       READ = 15, WRITE = 16, STATFS = 17, RELEASE = 18, FSYNC = 20, FLUSH = 25, INIT = 26, OPENDIR = 27,
       READDIR = 28, GETLK = 31, SETLK = 32, SETLKW = 33, ACCESS = 34, CREATE = 35, INTERRUPT = 36,
       RENAME2 = 45 };

/* What the mock host last received, for checking the guest's encoding. */
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t seen_opcode;
static unsigned char seen_body[512], seen_upload[256];
static size_t seen_length, seen_upload_length;
static int waiting_lock_cancelled;

static const char TOKEN[] = "0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0";
static char directory[256], file_path[512];
static const char *file_name = "notes # 1.txt";

/* MARK: Mock host — the same answers DropFileServer gives, from a temp file. */

static void put_attr(unsigned char out[NP_DROP_ATTR], uint8_t kind, uint32_t mode, uint64_t size) {
    struct np_drop_attr attr = {kind, mode, size, 1700000000, 5, 77};
    np_drop_attr_encode(out, &attr);
}
static int host_path(const unsigned char *body, size_t length, const char **name) {
    if (length < NP_DROP_TOKEN || memcmp(body, TOKEN, NP_DROP_TOKEN)) return NP_DROP_ENOENT;
    static __thread char buffer[NP_DROP_NAME_MAX + 1];
    memcpy(buffer, body + NP_DROP_TOKEN, length - NP_DROP_TOKEN); buffer[length - NP_DROP_TOKEN] = 0;
    *name = buffer;
    return buffer[0] && strcmp(buffer, file_name) && strcmp(buffer, "tmp") ? NP_DROP_ENOENT : 0;
}

struct work { struct np_file2_stream *stream; uint16_t opcode; unsigned char body[512]; size_t length; };
static void record(const struct work *work, const unsigned char *upload, size_t upload_length) {
    pthread_mutex_lock(&seen_lock);
    seen_opcode = work->opcode; memcpy(seen_body, work->body, work->length); seen_length = work->length;
    memcpy(seen_upload, upload, upload_length); seen_upload_length = upload_length;
    pthread_mutex_unlock(&seen_lock);
}
static int host_fd = -1;

static void *host_serve(void *argument) {
    struct work *work = argument;
    struct np_file2_stream *stream = work->stream;
    const unsigned char *body = work->body;
    unsigned char out[8 + NP_DROP_ATTR];
    const char *name = "";
    int status;
    switch (work->opcode) {
    case NP_DROP_LOOKUP:
        if ((status = host_path(body, work->length, &name))) { np_file2_respond(stream, status, NULL, 0, 1); break; }
        put_attr(out, name[0] ? NP_DROP_FILE : NP_DROP_DIRECTORY, 0644, name[0] ? 11 : 0);
        np_file2_respond(stream, 0, out, NP_DROP_ATTR, 1);
        break;
    case NP_DROP_LIST: {
        unsigned char entries[2 + NP_DROP_NAME_MAX + NP_DROP_ATTR];
        const char *listed = work->length ? file_name : TOKEN;
        size_t n = strlen(listed);
        if (work->length && (work->length != NP_DROP_TOKEN || memcmp(body, TOKEN, NP_DROP_TOKEN))) {
            np_file2_respond(stream, NP_DROP_ENOENT, NULL, 0, 1); break;
        }
        entries[0] = (unsigned char)n; entries[1] = 0; memcpy(entries + 2, listed, n);
        put_attr(entries + 2 + n, work->length ? NP_DROP_FILE : NP_DROP_DIRECTORY, 0644, 11);
        np_file2_respond(stream, 0, NULL, 0, 0);
        /* Split mid-entry: the guest must reassemble DATA frames. */
        np_file2_send(stream, entries, 5, 0);
        np_file2_send(stream, entries + 5, 2 + n + NP_DROP_ATTR - 5, 1);
        break;
    }
    case NP_DROP_OPEN:
        record(work, NULL, 0);
        if ((status = host_path(body + 4, work->length - 4, &name)) || !name[0]) { np_file2_respond(stream, status ? status : NP_DROP_EISDIR, NULL, 0, 1); break; }
        np_file_put64(out, 42);
        put_attr(out + 8, NP_DROP_FILE, 0644, 11);
        np_file2_respond(stream, 0, out, sizeof(out), 1);
        break;
    case NP_DROP_READ: {
        uint64_t handle = np_file_u64(body), offset = np_file_u64(body + 8);
        uint32_t length = np_file_u32(body + 16);
        if (handle != 42) { np_file2_respond(stream, NP_DROP_EBADF, NULL, 0, 1); break; }
        unsigned char *bytes = malloc(length ? length : 1);
        ssize_t n = pread(host_fd, bytes, length, (off_t)offset);
        np_file2_respond(stream, 0, NULL, 0, 0);
        np_file2_send(stream, bytes, n > 0 ? (size_t)n : 0, 1);
        free(bytes);
        break;
    }
    case NP_DROP_RELEASE:
        np_file2_respond(stream, np_file_u64(body) == 42 ? 0 : NP_DROP_EBADF, NULL, 0, 1);
        break;
    case NP_DROP_WRITE: {
        unsigned char upload[256]; size_t uploaded = 0;
        struct np_file2_message *message = malloc(sizeof(*message));
        while (np_file2_receive(stream, message) == 1) {
            memcpy(upload + uploaded, message->data, message->length); uploaded += message->length;
        }
        free(message);
        record(work, upload, uploaded);
        np_file_put32(out, (uint32_t)uploaded);
        np_file2_respond(stream, 0, out, 4, 1);
        break;
    }
    case NP_DROP_SETATTR:
        record(work, NULL, 0);
        put_attr(out, NP_DROP_FILE, 0600, np_file_u32(body) & NP_DROP_SET_SIZE ? np_file_u64(body + 8) : 11);
        np_file2_respond(stream, 0, out, NP_DROP_ATTR, 1);
        break;
    case NP_DROP_CREATE:
        record(work, NULL, 0);
        np_file_put64(out, 43);
        put_attr(out + 8, NP_DROP_FILE, np_file_u32(body + 4), 0);
        np_file2_respond(stream, 0, out, sizeof(out), 1);
        break;
    case NP_DROP_UNLINK: case NP_DROP_RENAME: case NP_DROP_FSYNC: case NP_DROP_FLUSH:
        record(work, NULL, 0);
        np_file2_respond(stream, 0, NULL, 0, 1);
        break;
    case NP_DROP_GETLK: {
        record(work, NULL, 0);
        unsigned char conflict[24];
        np_file_put64(conflict, 100); np_file_put64(conflict + 8, 199);
        np_file_put32(conflict + 16, 1 /* F_WRLCK */); np_file_put32(conflict + 20, 0);
        np_file2_respond(stream, 0, conflict, sizeof(conflict), 1);
        break;
    }
    case NP_DROP_SETLK:
        record(work, NULL, 0);
        if (np_file_u32(body + 36) & NP_DROP_LOCK_WAIT) {
            /* Held by "a Mac program" until the guest gives up. */
            while (!np_file2_stream_cancelled(stream)) usleep(5000);
            pthread_mutex_lock(&seen_lock); waiting_lock_cancelled = 1; pthread_mutex_unlock(&seen_lock);
            break;
        }
        np_file2_respond(stream, 0, NULL, 0, 1);
        break;
    default:
        np_file2_respond(stream, NP_DROP_EINVAL, NULL, 0, 1);
    }
    struct np_file2_message *message = malloc(sizeof(*message));
    while (np_file2_receive(stream, message) == 1) {}
    free(message);
    np_file2_stream_release(stream);
    free(work);
    return NULL;
}
static void on_request(void *context, struct np_file2_stream *stream, uint16_t opcode, const void *body, size_t length) {
    (void)context;
    struct work *work = calloc(1, sizeof(*work));
    assert(work && length <= sizeof(work->body));
    work->stream = stream; work->opcode = opcode; memcpy(work->body, body, length); work->length = length;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, host_serve, work) == 0);
    pthread_detach(thread);
}

/* MARK: FUSE driver */

struct captured { unsigned char bytes[256 * 1024]; size_t length; int replies; };
static int capture(void *context, const struct iovec *vector, int count) {
    struct captured *out = context;
    out->length = 0; out->replies++;
    for (int i = 0; i < count; i++) {
        assert(out->length + vector[i].iov_len <= sizeof(out->bytes));
        memcpy(out->bytes + out->length, vector[i].iov_base, vector[i].iov_len);
        out->length += vector[i].iov_len;
    }
    return 0;
}
static uint64_t next_unique = 1;
/* Sends one request; returns the reply's error (negative errno) and body. */
static int call(struct np_drop_fs *fs, uint32_t opcode, uint64_t node, const void *body, size_t length,
                struct captured *out) {
    unsigned char request[4096];
    struct in_header in = {(uint32_t)(sizeof(in) + length), opcode, next_unique++, node, 1000, 1000, 1, 0, 0};
    memcpy(request, &in, sizeof(in)); memcpy(request + sizeof(in), body, length);
    out->replies = 0;
    assert(np_drop_fs_handle(fs, request, sizeof(in) + length, capture, out) == 0);
    if (opcode == FORGET) { assert(out->replies == 0); return 0; }
    assert(out->replies == 1 && out->length >= sizeof(struct out_header));
    struct out_header header; memcpy(&header, out->bytes, sizeof(header));
    assert(header.unique == in.unique && header.len == out->length);
    assert(header.error ? out->length == sizeof(header) : 1);
    return header.error;
}
static const void *reply_body(struct captured *out) { return out->bytes + sizeof(struct out_header); }
static size_t reply_length(struct captured *out) { return out->length - sizeof(struct out_header); }

static int lookup(struct np_drop_fs *fs, uint64_t parent, const char *name, struct entry_out *entry, struct captured *out) {
    int error = call(fs, LOOKUP, parent, name, strlen(name) + 1, out);
    if (!error) { assert(reply_length(out) == sizeof(*entry)); memcpy(entry, reply_body(out), sizeof(*entry)); }
    return error;
}

static size_t list(struct np_drop_fs *fs, uint64_t node, uint64_t offset, uint32_t size, char names[][64], struct captured *out) {
    struct read_in read = {0, offset, size, 0, 0, 0, 0};
    assert(call(fs, READDIR, node, &read, sizeof(read), out) == 0);
    size_t count = 0, position = 0;
    const unsigned char *bytes = reply_body(out);
    while (position < reply_length(out)) {
        struct dirent_head head; memcpy(&head, bytes + position, sizeof(head));
        memcpy(names[count], bytes + position + sizeof(head), head.namelen); names[count][head.namelen] = 0;
        assert(head.off == offset + count + 1);
        count++;
        position += (sizeof(head) + head.namelen + 7) & ~(size_t)7;
    }
    assert(position == reply_length(out));
    return count;
}

struct interrupt_after { struct np_drop_fs *fs; uint64_t unique; };
static void *send_interrupt(void *argument) {
    struct interrupt_after *after = argument;
    usleep(50000);
    unsigned char request[48] = {0};
    struct in_header in = {sizeof(request), INTERRUPT, 999999, 0, 0, 0, 0, 0, 0};
    memcpy(request, &in, sizeof(in)); np_file_put64(request + sizeof(in), after->unique);
    struct captured *none = malloc(sizeof(*none));
    none->replies = 0;
    assert(np_drop_fs_handle(after->fs, request, sizeof(request), capture, none) == 0 && none->replies == 0);
    free(none);
    return NULL;
}

static void modifying_operations(struct np_drop_fs *fs, uint64_t token_node, uint64_t file_node, struct captured *out) {
    /* WRITE: the bytes travel as DATA after the request. */
    unsigned char write[40 + 3] = {0};
    np_file_put64(write, 42); np_file_put64(write + 8, 7); np_file_put32(write + 16, 3);
    memcpy(write + 40, "abc", 3);
    assert(call(fs, WRITE, file_node, write, sizeof(write), out) == 0);
    assert(reply_length(out) == 8 && np_file_u32(reply_body(out)) == 3);
    assert(seen_opcode == NP_DROP_WRITE && np_file_u64(seen_body) == 42 && np_file_u64(seen_body + 8) == 7 &&
           seen_upload_length == 3 && !memcmp(seen_upload, "abc", 3));
    np_file_put32(write + 16, 4); /* claims more than it carries */
    assert(call(fs, WRITE, file_node, write, sizeof(write), out) == -NP_DROP_EINVAL);

    /* SETATTR: size, mtime and the open file; ownership stays the mount's. */
    unsigned char set[88] = {0};
    np_file_put32(set, NP_DROP_SET_SIZE | NP_DROP_SET_MTIME | NP_DROP_SET_HANDLE | 512 /* lock owner */);
    np_file_put64(set + 8, 42); np_file_put64(set + 16, 5); np_file_put64(set + 40, 1234);
    np_file_put32(set + 60, 9);
    assert(call(fs, SETATTR, file_node, set, sizeof(set), out) == 0);
    struct attr_out attr; memcpy(&attr, reply_body(out), sizeof(attr));
    assert(attr.attr.size == 5);
    assert(seen_opcode == NP_DROP_SETATTR &&
           np_file_u32(seen_body) == (NP_DROP_SET_SIZE | NP_DROP_SET_MTIME | NP_DROP_SET_HANDLE) &&
           np_file_u64(seen_body + 8) == 5 && np_file_u64(seen_body + 28) == 1234 &&
           np_file_u32(seen_body + 36) == 9 && np_file_u64(seen_body + 40) == 42 &&
           !memcmp(seen_body + 48, TOKEN, NP_DROP_TOKEN));
    np_file_put32(set, NP_DROP_SET_UID); np_file_put32(set + 76, 0);
    assert(call(fs, SETATTR, file_node, set, sizeof(set), out) == -NP_DROP_EPERM);
    np_file_put32(set + 76, 1000); /* chown to the owner is a no-op */
    assert(call(fs, SETATTR, file_node, set, sizeof(set), out) == 0 && np_file_u32(seen_body) == 0);
    assert(call(fs, SETATTR, token_node, set, sizeof(set), out) == -NP_DROP_EPERM);

    /* fstat asks about the open file, not the path. */
    unsigned char getattr[16] = {0}; np_file_put32(getattr, 1); np_file_put64(getattr + 8, 42);
    assert(call(fs, GETATTR, file_node, getattr, sizeof(getattr), out) == 0);
    assert(seen_opcode == NP_DROP_SETATTR && np_file_u32(seen_body) == NP_DROP_SET_HANDLE && np_file_u64(seen_body + 40) == 42);

    /* CREATE answers with an entry and an open file. */
    unsigned char create[16 + 4] = {0};
    np_file_put32(create, 0101 /* O_WRONLY|O_CREAT */); np_file_put32(create + 4, 0666); np_file_put32(create + 8, 022);
    memcpy(create + 16, "tmp", 4);
    assert(call(fs, CREATE, token_node, create, sizeof(create), out) == 0);
    assert(reply_length(out) == sizeof(struct entry_out) + sizeof(struct open_out));
    struct entry_out created; memcpy(&created, reply_body(out), sizeof(created));
    struct open_out created_open; memcpy(&created_open, (const unsigned char *)reply_body(out) + sizeof(created), sizeof(created_open));
    assert(created.nodeid > 1 && created.attr.mode == (0100000 | 0644) && created_open.fh == 43);
    assert(np_file_u32(seen_body) == 0101 && np_file_u32(seen_body + 4) == 0644);
    assert(call(fs, CREATE, 1, create, sizeof(create), out) == -NP_DROP_EPERM);
    assert(call(fs, CREATE, file_node, create, sizeof(create), out) == -ENOTDIR);

    /* RENAME tmp over the dropped name: the moved node answers to the new name,
     * and the replaced one is no longer reachable by path. */
    unsigned char rename[16 + 4 + 14] = {0};
    np_file_put64(rename, token_node); np_file_put32(rename + 8, NP_DROP_RENAME_NOREPLACE);
    memcpy(rename + 16, "tmp", 4); memcpy(rename + 20, file_name, strlen(file_name) + 1);
    assert(call(fs, RENAME2, token_node, rename, 20 + strlen(file_name) + 1, out) == 0);
    assert(seen_opcode == NP_DROP_RENAME && np_file_u32(seen_body) == NP_DROP_RENAME_NOREPLACE &&
           seen_body[4] == 3 && !memcmp(seen_body + 6, TOKEN, NP_DROP_TOKEN) &&
           !memcmp(seen_body + 6 + NP_DROP_TOKEN, "tmp", 3) &&
           !memcmp(seen_body + 9 + NP_DROP_TOKEN, file_name, strlen(file_name)));
    assert(call(fs, GETATTR, file_node, (unsigned char[16]){0}, 16, out) == -NP_DROP_ENOENT);
    assert(call(fs, GETATTR, created.nodeid, (unsigned char[16]){0}, 16, out) == 0);
    struct entry_out found;
    assert(lookup(fs, token_node, file_name, &found, out) == 0 && found.nodeid == created.nodeid);
    np_file_put64(rename, file_node);
    assert(call(fs, RENAME2, token_node, rename, 20 + strlen(file_name) + 1, out) == -NP_DROP_EXDEV);
    np_file_put64(rename, token_node); np_file_put32(rename + 8, NP_DROP_RENAME_EXCHANGE);
    assert(call(fs, RENAME2, token_node, rename, 20 + strlen(file_name) + 1, out) == -NP_DROP_EINVAL);

    /* UNLINK: the node leaves the name. */
    assert(call(fs, UNLINK, token_node, file_name, strlen(file_name) + 1, out) == 0);
    assert(seen_opcode == NP_DROP_UNLINK && !memcmp(seen_body + NP_DROP_TOKEN, file_name, strlen(file_name)));
    assert(call(fs, GETATTR, created.nodeid, (unsigned char[16]){0}, 16, out) == -NP_DROP_ENOENT);

    unsigned char fsync[16] = {0}; np_file_put64(fsync, 42);
    assert(call(fs, FSYNC, file_node, fsync, sizeof(fsync), out) == 0 && seen_opcode == NP_DROP_FSYNC);
    unsigned char flush[24] = {0}; np_file_put64(flush, 42); np_file_put64(flush + 16, 77);
    assert(call(fs, FLUSH, file_node, flush, sizeof(flush), out) == 0);
    assert(seen_opcode == NP_DROP_FLUSH && np_file_u64(seen_body) == 42 && np_file_u64(seen_body + 8) == 77);

    /* Locks: the request is forwarded; GETLK reports the host's conflict. */
    unsigned char lock[48] = {0};
    np_file_put64(lock, 42); np_file_put64(lock + 8, 77);
    np_file_put64(lock + 16, 0); np_file_put64(lock + 24, 9); np_file_put32(lock + 32, 1);
    assert(call(fs, GETLK, file_node, lock, sizeof(lock), out) == 0 && reply_length(out) == 24);
    const unsigned char *conflict = reply_body(out);
    assert(np_file_u64(conflict) == 100 && np_file_u64(conflict + 8) == 199 && np_file_u32(conflict + 16) == 1);
    assert(seen_opcode == NP_DROP_GETLK && np_file_u64(seen_body + 24) == 9 && np_file_u32(seen_body + 36) == 0);
    np_file_put32(lock + 40, 1 /* FUSE_LK_FLOCK */);
    assert(call(fs, SETLK, file_node, lock, sizeof(lock), out) == 0);
    assert(seen_opcode == NP_DROP_SETLK && np_file_u32(seen_body + 36) == NP_DROP_LOCK_FLOCK);
    np_file_put32(lock + 32, 3);
    assert(call(fs, SETLK, file_node, lock, sizeof(lock), out) == -NP_DROP_EINVAL);
    np_file_put32(lock + 32, 1); np_file_put32(lock + 40, 0);

    /* SETLKW waits until INTERRUPT cancels it... */
    struct interrupt_after after = {fs, next_unique};
    pthread_t thread; assert(pthread_create(&thread, NULL, send_interrupt, &after) == 0);
    assert(call(fs, SETLKW, file_node, lock, sizeof(lock), out) == -NP_DROP_EINTR);
    assert(pthread_join(thread, NULL) == 0);
    int cancelled = 0;
    for (int i = 0; i < 200 && !cancelled; i++) {
        pthread_mutex_lock(&seen_lock); cancelled = waiting_lock_cancelled; pthread_mutex_unlock(&seen_lock);
        if (!cancelled) usleep(5000);
    }
    assert(cancelled && (np_file_u32(seen_body + 36) & NP_DROP_LOCK_WAIT));
    /* ...including an INTERRUPT that overtakes its request. */
    after.unique = next_unique; after.fs = fs;
    unsigned char early[48] = {0};
    struct in_header in = {sizeof(early), INTERRUPT, 999998, 0, 0, 0, 0, 0, 0};
    memcpy(early, &in, sizeof(in)); np_file_put64(early + sizeof(in), after.unique);
    assert(np_drop_fs_handle(fs, early, sizeof(early), capture, out) == 0);
    assert(call(fs, SETLKW, file_node, lock, sizeof(lock), out) == -NP_DROP_EINTR);
}

int main(void) {
    strcpy(directory, "/tmp/np-drop-test-XXXXXX");
    assert(mkdtemp(directory));
    snprintf(file_path, sizeof(file_path), "%s/file", directory);
    int fd = open(file_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0 && write(fd, "hello drops", 11) == 11);
    host_fd = fd;

    struct np_drop_fs *fs = np_drop_fs_create(1000, 1000);
    struct captured *out = malloc(sizeof(*out));
    assert(fs && out);
    char names[8][64];

    /* Without a host: nothing to find, an empty root, and no crash. */
    struct entry_out entry;
    assert(lookup(fs, 1, TOKEN, &entry, out) == -NP_DROP_ENOENT);
    assert(list(fs, 1, 0, 4096, names, out) == 2 && !strcmp(names[0], ".") && !strcmp(names[1], ".."));

    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct np_file2_handlers handlers = {NULL, on_request, NULL, NULL};
    struct np_file2_session *host = np_file2_session_start(sockets[0], NP_FILE2_ROLE_HOST, &handlers);
    struct np_file2_session *guest = np_file2_session_start(sockets[1], NP_FILE2_ROLE_GUEST, NULL);
    assert(host && guest);
    np_drop_fs_set_session(fs, guest);

    unsigned char init[64] = {0};
    np_file_put32(init, 7); np_file_put32(init + 4, 41); np_file_put32(init + 8, 1u << 20); np_file_put32(init + 12, 0xffffffffu);
    assert(call(fs, INIT, 0, init, sizeof(init), out) == 0 && reply_length(out) == 64);
    const unsigned char *reply = reply_body(out);
    assert(np_file_u32(reply) == 7 && np_file_u32(reply + 4) == 31 && np_file_u32(reply + 8) == NP_DROP_MAX_READ);
    /* Async reads, POSIX and flock locks, atomic O_TRUNC and big writes. */
    assert(np_file_u32(reply + 12) == (1u | 2u | 8u | 32u | 1024u));
    assert(np_file_u32(reply + 20) == NP_DROP_MAX_WRITE);

    assert(lookup(fs, 1, "not-a-token", &entry, out) == -NP_DROP_ENOENT);
    assert(lookup(fs, 1, "00000000-0000-0000-0000-000000000000", &entry, out) == -NP_DROP_ENOENT);
    assert(lookup(fs, 1, TOKEN, &entry, out) == 0);
    uint64_t token_node = entry.nodeid;
    assert(token_node > 1 && (entry.attr.mode & 0170000) == 0040000 && entry.attr.uid == 1000);
    assert(entry.entry_valid == 0 && entry.attr_valid == 0);
    assert(lookup(fs, token_node, "other", &entry, out) == -NP_DROP_ENOENT);
    assert(lookup(fs, token_node, "..", &entry, out) == -NP_DROP_ENOENT);
    assert(lookup(fs, token_node, file_name, &entry, out) == 0);
    uint64_t file_node = entry.nodeid;
    assert(entry.attr.mode == (0100000 | 0644) && entry.attr.size == 11 && entry.attr.mtime == 1700000000);
    assert(lookup(fs, file_node, "x", &entry, out) == -ENOTDIR);
    /* Repeated lookups keep the same node. */
    assert(lookup(fs, token_node, file_name, &entry, out) == 0 && entry.nodeid == file_node);

    assert(call(fs, GETATTR, file_node, (unsigned char[16]){0}, 16, out) == 0);
    struct attr_out attr; memcpy(&attr, reply_body(out), sizeof(attr));
    assert(attr.attr.size == 11 && attr.attr.ino == file_node);
    assert(call(fs, GETATTR, 1, (unsigned char[16]){0}, 16, out) == 0);

    uint32_t open_flags[2] = {02 | 01000 /* O_RDWR | O_TRUNC */, 0};
    assert(call(fs, OPEN, file_node, open_flags, sizeof(open_flags), out) == 0);
    assert(seen_opcode == NP_DROP_OPEN && np_file_u32(seen_body) == (02 | 01000) &&
           !memcmp(seen_body + 4, TOKEN, NP_DROP_TOKEN));
    open_flags[0] = 0;
    assert(call(fs, OPEN, token_node, open_flags, sizeof(open_flags), out) == -NP_DROP_EISDIR);
    assert(call(fs, OPEN, file_node, open_flags, sizeof(open_flags), out) == 0);
    struct open_out opened; memcpy(&opened, reply_body(out), sizeof(opened));
    assert(opened.fh == 42 && opened.flags == 0);

    struct read_in read = {opened.fh, 6, 100, 0, 0, 0, 0};
    assert(call(fs, READ, file_node, &read, sizeof(read), out) == 0);
    assert(reply_length(out) == 5 && !memcmp(reply_body(out), "drops", 5));
    read.offset = 11;
    assert(call(fs, READ, file_node, &read, sizeof(read), out) == 0 && reply_length(out) == 0);
    read.fh = 7;
    assert(call(fs, READ, file_node, &read, sizeof(read), out) == -NP_DROP_EBADF);
    unsigned char release[24] = {0}; np_file_put64(release, 42);
    assert(call(fs, RELEASE, file_node, release, sizeof(release), out) == 0);

    assert(call(fs, OPENDIR, file_node, open_flags, sizeof(open_flags), out) == -ENOTDIR);
    assert(call(fs, OPENDIR, token_node, open_flags, sizeof(open_flags), out) == 0);
    assert(list(fs, 1, 0, 4096, names, out) == 3 && !strcmp(names[2], TOKEN));
    assert(list(fs, token_node, 0, 4096, names, out) == 3 && !strcmp(names[2], file_name));
    /* Resuming at an offset, and a buffer that holds a single entry. */
    assert(list(fs, token_node, 2, 4096, names, out) == 1 && !strcmp(names[0], file_name));
    assert(list(fs, token_node, 0, 32, names, out) == 1 && !strcmp(names[0], "."));

    assert(call(fs, STATFS, 1, NULL, 0, out) == 0);
    assert(call(fs, MKDIR, token_node, "d", 2, out) == -NP_DROP_EPERM);
    assert(call(fs, ACCESS, file_node, NULL, 0, out) == -38 /* ENOSYS */);
    modifying_operations(fs, token_node, file_node, out);

    /* FORGET drops the node after every lookup is released. The renames above
     * retired file_node, so look the name up afresh, twice. */
    assert(lookup(fs, token_node, file_name, &entry, out) == 0 && entry.nodeid != file_node);
    assert(lookup(fs, token_node, file_name, &entry, out) == 0);
    file_node = entry.nodeid;
    uint64_t forget = 1;
    call(fs, FORGET, file_node, &forget, sizeof(forget), out);
    assert(call(fs, GETATTR, file_node, (unsigned char[16]){0}, 16, out) == 0);
    call(fs, FORGET, file_node, &forget, sizeof(forget), out);
    assert(call(fs, GETATTR, file_node, (unsigned char[16]){0}, 16, out) == -NP_DROP_ENOENT);

    /* Malformed: the header length must match the request. */
    struct in_header bad = {99, GETATTR, 1, 1, 0, 0, 0, 0, 0};
    assert(np_drop_fs_handle(fs, &bad, sizeof(bad), capture, out) == -1);

    /* The host going away: known drops become unreachable, never stale data. */
    np_drop_fs_set_session(fs, NULL);
    np_file2_session_close(guest);
    np_file2_session_close(host);
    assert(call(fs, GETATTR, token_node, (unsigned char[16]){0}, 16, out) == -NP_DROP_EIO);
    assert(lookup(fs, 1, TOKEN, &entry, out) == -NP_DROP_ENOENT);
    assert(call(fs, 38 /* DESTROY */, 0, NULL, 0, out) == 0 && np_drop_fs_destroyed(fs));

    np_drop_fs_destroy(fs);
    free(out);
    close(fd); unlink(file_path); rmdir(directory);
    puts("drop filesystem tests passed");
    return 0;
}
