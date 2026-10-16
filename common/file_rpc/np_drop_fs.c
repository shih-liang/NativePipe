#define _GNU_SOURCE
#include "np_drop_fs.h"
#include "np_file_rpc.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* FUSE kernel ABI subset (stable since protocol 7.12). Defined here so the
 * handler builds and is tested on any host; Linux builds check the layouts
 * against <linux/fuse.h>. Request and reply values are Linux's. */
struct npf_in_header {
    uint32_t len, opcode;
    uint64_t unique, nodeid;
    uint32_t uid, gid, pid;
    uint16_t total_extlen, padding;
};
struct npf_out_header { uint32_t len; int32_t error; uint64_t unique; };
struct npf_attr {
    uint64_t ino, size, blocks, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec, mode, nlink, uid, gid, rdev, blksize, flags;
};
struct npf_entry_out {
    uint64_t nodeid, generation, entry_valid, attr_valid;
    uint32_t entry_valid_nsec, attr_valid_nsec;
    struct npf_attr attr;
};
struct npf_attr_out { uint64_t attr_valid; uint32_t attr_valid_nsec, dummy; struct npf_attr attr; };
struct npf_init_in { uint32_t major, minor, max_readahead, flags; };
struct npf_init_out {
    uint32_t major, minor, max_readahead, flags;
    uint16_t max_background, congestion_threshold;
    uint32_t max_write, time_gran;
    uint16_t max_pages, map_alignment;
    uint32_t flags2, max_stack_depth;
    uint16_t request_timeout, unused[11];
};
struct npf_open_in { uint32_t flags, open_flags; };
struct npf_open_out { uint64_t fh; uint32_t open_flags; int32_t backing_id; };
struct npf_read_in { uint64_t fh, offset; uint32_t size, read_flags; uint64_t lock_owner; uint32_t flags, padding; };
struct npf_release_in { uint64_t fh; uint32_t flags, release_flags; uint64_t lock_owner; };
struct npf_forget_in { uint64_t nlookup; };
struct npf_batch_forget_in { uint32_t count, dummy; };
struct npf_forget_one { uint64_t nodeid, nlookup; };
struct npf_dirent { uint64_t ino, off; uint32_t namelen, type; };
struct npf_kstatfs {
    uint64_t blocks, bfree, bavail, files, ffree;
    uint32_t bsize, namelen, frsize, padding, spare[6];
};

#if defined(__linux__) && defined(__has_include)
#if __has_include(<linux/fuse.h>)
#include <linux/fuse.h>
_Static_assert(sizeof(struct npf_in_header) == sizeof(struct fuse_in_header), "fuse_in_header");
_Static_assert(sizeof(struct npf_out_header) == sizeof(struct fuse_out_header), "fuse_out_header");
_Static_assert(sizeof(struct npf_attr) == sizeof(struct fuse_attr), "fuse_attr");
_Static_assert(sizeof(struct npf_entry_out) == sizeof(struct fuse_entry_out), "fuse_entry_out");
_Static_assert(sizeof(struct npf_attr_out) == sizeof(struct fuse_attr_out), "fuse_attr_out");
_Static_assert(sizeof(struct npf_open_out) == sizeof(struct fuse_open_out), "fuse_open_out");
_Static_assert(sizeof(struct npf_read_in) == sizeof(struct fuse_read_in), "fuse_read_in");
_Static_assert(sizeof(struct npf_release_in) == sizeof(struct fuse_release_in), "fuse_release_in");
_Static_assert(sizeof(struct npf_dirent) == sizeof(struct fuse_dirent), "fuse_dirent");
_Static_assert(sizeof(struct npf_kstatfs) == sizeof(struct fuse_kstatfs), "fuse_kstatfs");
#endif
#endif
_Static_assert(sizeof(struct npf_in_header) == 40, "in header");
_Static_assert(sizeof(struct npf_attr) == 88, "attr");
_Static_assert(sizeof(struct npf_init_out) == 64, "init out");

enum {
    NPF_LOOKUP = 1, NPF_FORGET = 2, NPF_GETATTR = 3, NPF_SETATTR = 4, NPF_READLINK = 5,
    NPF_SYMLINK = 6, NPF_MKNOD = 8, NPF_MKDIR = 9, NPF_UNLINK = 10, NPF_RMDIR = 11,
    NPF_RENAME = 12, NPF_LINK = 13, NPF_OPEN = 14, NPF_READ = 15, NPF_WRITE = 16,
    NPF_STATFS = 17, NPF_RELEASE = 18, NPF_FSYNC = 20, NPF_SETXATTR = 21,
    NPF_REMOVEXATTR = 24, NPF_FLUSH = 25, NPF_INIT = 26, NPF_OPENDIR = 27,
    NPF_READDIR = 28, NPF_RELEASEDIR = 29, NPF_FSYNCDIR = 30, NPF_CREATE = 35,
    NPF_INTERRUPT = 36, NPF_DESTROY = 38, NPF_BATCH_FORGET = 42, NPF_FALLOCATE = 43,
    NPF_RENAME2 = 45, NPF_COPY_FILE_RANGE = 47, NPF_TMPFILE = 51,
};
/* Linux values, independent of the build host's headers. */
#define NPF_ENOSYS 38
#define NPF_ASYNC_READ 1u
#define NPF_O_ACCMODE 03u
#define NPF_O_TRUNC 01000u
#define NPF_S_IFDIR 0040000u
#define NPF_S_IFREG 0100000u
#define NPF_DT_DIR 4u
#define NPF_DT_REG 8u
#define ROOT_ID 1u
/* LIST results are bounded; drops are a handful of user-chosen files. */
#define LIST_MAX (1024u * 1024u)

struct node {
    struct node *next;
    uint64_t id, lookups;
    uint8_t kind;
    char token[NP_DROP_TOKEN + 1];
    char name[NP_DROP_NAME_MAX + 1];
};

struct np_drop_fs {
    pthread_mutex_t lock;
    pthread_cond_t idle;
    struct np_file2_session *session;
    unsigned generation;
    unsigned users[2];
    struct node *nodes;
    uint64_t next_id;
    uid_t owner;
    gid_t group;
    int destroyed;
};

void np_drop_attr_encode(unsigned char out[NP_DROP_ATTR], const struct np_drop_attr *attr) {
    memset(out, 0, NP_DROP_ATTR);
    out[0] = attr->kind;
    np_file_put32(out + 4, attr->mode);
    np_file_put64(out + 8, attr->size);
    np_file_put64(out + 16, (uint64_t)attr->mtime_seconds);
    np_file_put32(out + 24, attr->mtime_nanoseconds);
    np_file_put64(out + 32, attr->file_id);
}
int np_drop_attr_decode(const unsigned char *in, size_t length, struct np_drop_attr *attr) {
    if (length < NP_DROP_ATTR || (in[0] != NP_DROP_DIRECTORY && in[0] != NP_DROP_FILE) ||
        np_file_u32(in + 24) >= 1000000000u) { errno = EPROTO; return -1; }
    attr->kind = in[0];
    attr->mode = np_file_u32(in + 4) & 07777u;
    attr->size = np_file_u64(in + 8);
    attr->mtime_seconds = (int64_t)np_file_u64(in + 16);
    attr->mtime_nanoseconds = np_file_u32(in + 24);
    attr->file_id = np_file_u64(in + 32);
    return 0;
}

struct np_drop_fs *np_drop_fs_create(uid_t owner, gid_t group) {
    struct np_drop_fs *fs = calloc(1, sizeof(*fs));
    if (!fs) return NULL;
    pthread_mutex_init(&fs->lock, NULL);
    pthread_cond_init(&fs->idle, NULL);
    fs->next_id = ROOT_ID + 1;
    fs->owner = owner; fs->group = group;
    return fs;
}
void np_drop_fs_destroy(struct np_drop_fs *fs) {
    if (!fs) return;
    while (fs->nodes) { struct node *next = fs->nodes->next; free(fs->nodes); fs->nodes = next; }
    pthread_cond_destroy(&fs->idle);
    pthread_mutex_destroy(&fs->lock);
    free(fs);
}
void np_drop_fs_set_session(struct np_drop_fs *fs, struct np_file2_session *session) {
    pthread_mutex_lock(&fs->lock);
    unsigned previous = fs->generation++ & 1u;
    fs->session = session;
    while (fs->users[previous]) pthread_cond_wait(&fs->idle, &fs->lock);
    pthread_mutex_unlock(&fs->lock);
}
int np_drop_fs_destroyed(struct np_drop_fs *fs) {
    pthread_mutex_lock(&fs->lock);
    int destroyed = fs->destroyed;
    pthread_mutex_unlock(&fs->lock);
    return destroyed;
}

/* MARK: Host calls */

struct call {
    uint32_t status;
    unsigned char head[8 + NP_DROP_ATTR];
    size_t head_length;
    unsigned char *data;   /* optional, caller-owned */
    size_t data_capacity, data_length;
};

/* Returns 0 with the host's status, or -1 if the host is unreachable or broke
 * the protocol; the caller then answers EIO (or ENOENT for name lookups). */
static int call_host(struct np_drop_fs *fs, uint16_t opcode, const void *body, size_t length, struct call *call) {
    pthread_mutex_lock(&fs->lock);
    struct np_file2_session *session = fs->session;
    unsigned generation = fs->generation & 1u;
    if (session) fs->users[generation]++;
    pthread_mutex_unlock(&fs->lock);
    if (!session) { errno = ENOTCONN; return -1; }
    int result = -1;
    struct np_file2_message *message = malloc(sizeof(*message));
    struct np_file2_stream *stream = message ? np_file2_request(session, opcode, body, length, 1) : NULL;
    if (stream && np_file2_receive(stream, message) == 1 && message->type == NP_FILE2_RESPONSE &&
        message->length <= sizeof(call->head)) {
        call->status = message->status;
        call->head_length = message->length;
        memcpy(call->head, message->data, message->length);
        call->data_length = 0;
        int received;
        while ((received = np_file2_receive(stream, message)) == 1) {
            if (!call->data || message->length > call->data_capacity - call->data_length) { received = -1; break; }
            memcpy(call->data + call->data_length, message->data, message->length);
            call->data_length += message->length;
        }
        if (received == 0) result = 0;
    }
    np_file2_stream_release(stream);
    free(message);
    pthread_mutex_lock(&fs->lock);
    if (--fs->users[generation] == 0) pthread_cond_broadcast(&fs->idle);
    pthread_mutex_unlock(&fs->lock);
    if (result < 0) errno = EIO;
    return result;
}

static size_t path_body(unsigned char *body, const char *token, const char *name) {
    size_t name_length = strlen(name);
    memcpy(body, token, NP_DROP_TOKEN);
    memcpy(body + NP_DROP_TOKEN, name, name_length);
    return NP_DROP_TOKEN + name_length;
}

/* Returns a Linux errno (0 on success). */
static int host_lookup(struct np_drop_fs *fs, const char *token, const char *name, struct np_drop_attr *attr) {
    unsigned char body[NP_DROP_TOKEN + NP_DROP_NAME_MAX];
    struct call call = {0};
    if (call_host(fs, NP_DROP_LOOKUP, body, path_body(body, token, name), &call) < 0) return NP_DROP_EIO;
    if (call.status) return (int)call.status;
    if (np_drop_attr_decode(call.head, call.head_length, attr) < 0) return NP_DROP_EIO;
    if (attr->kind != (name[0] ? NP_DROP_FILE : NP_DROP_DIRECTORY)) return NP_DROP_EIO;
    return 0;
}

/* MARK: Nodes */

static int valid_token(const char *name, size_t length) {
    if (length != NP_DROP_TOKEN) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        int dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}
static int valid_name(const char *name, size_t length) {
    return length && length <= NP_DROP_NAME_MAX && !memchr(name, '/', length) &&
           strcmp(name, ".") && strcmp(name, "..");
}

/* Locked. */
static struct node *find_node(struct np_drop_fs *fs, uint64_t id) {
    for (struct node *node = fs->nodes; node; node = node->next) if (node->id == id) return node;
    return NULL;
}
/* Copies a node's identity so no pointer outlives the lock. */
static int read_node(struct np_drop_fs *fs, uint64_t id, struct node *out) {
    if (id == ROOT_ID) { memset(out, 0, sizeof(*out)); out->id = ROOT_ID; return 0; }
    pthread_mutex_lock(&fs->lock);
    struct node *node = find_node(fs, id);
    if (node) *out = *node;
    pthread_mutex_unlock(&fs->lock);
    return node ? 0 : -1;
}
static uint64_t remember(struct np_drop_fs *fs, uint8_t kind, const char *token, const char *name) {
    pthread_mutex_lock(&fs->lock);
    struct node *node = fs->nodes;
    while (node && !(node->kind == kind && !strcmp(node->token, token) && !strcmp(node->name, name))) node = node->next;
    if (!node && (node = calloc(1, sizeof(*node)))) {
        node->id = fs->next_id++; node->kind = kind;
        strcpy(node->token, token); strcpy(node->name, name);
        node->next = fs->nodes; fs->nodes = node;
    }
    uint64_t id = 0;
    if (node) { node->lookups++; id = node->id; }
    pthread_mutex_unlock(&fs->lock);
    return id;
}
static void forget(struct np_drop_fs *fs, uint64_t id, uint64_t count) {
    pthread_mutex_lock(&fs->lock);
    struct node **link = &fs->nodes;
    while (*link && (*link)->id != id) link = &(*link)->next;
    if (*link) {
        struct node *node = *link;
        node->lookups = count >= node->lookups ? 0 : node->lookups - count;
        if (!node->lookups) { *link = node->next; free(node); }
    }
    pthread_mutex_unlock(&fs->lock);
}

/* MARK: Replies */

static int reply(np_drop_reply send, void *context, uint64_t unique, int error,
                 const void *body, size_t length) {
    struct npf_out_header header = {(uint32_t)(sizeof(header) + (error ? 0 : length)), -error, unique};
    struct iovec vector[2] = {{&header, sizeof(header)}, {(void *)body, error ? 0 : length}};
    return send(context, vector, error || !length ? 1 : 2);
}

static void fill_attr(const struct np_drop_fs *fs, struct npf_attr *out, uint64_t ino, const struct np_drop_attr *attr) {
    memset(out, 0, sizeof(*out));
    int directory = attr->kind == NP_DROP_DIRECTORY;
    out->ino = ino;
    out->size = directory ? 0 : attr->size;
    out->blocks = (out->size + 511) / 512;
    out->atime = out->mtime = out->ctime = (uint64_t)attr->mtime_seconds;
    out->atimensec = out->mtimensec = out->ctimensec = attr->mtime_nanoseconds;
    /* Read-only: the host's write bits are not offered. */
    out->mode = (directory ? NPF_S_IFDIR : NPF_S_IFREG) | (attr->mode & 0555u);
    out->nlink = directory ? 2 : 1;
    out->uid = (uint32_t)fs->owner; out->gid = (uint32_t)fs->group;
    out->blksize = 4096;
}
static const struct np_drop_attr root_attr = {NP_DROP_DIRECTORY, 0555, 0, 0, 0, 0};

/* Entry and attribute timeouts are zero: the host's file is checked on every
 * lookup and stat, exactly as a second Mac app would see it. */
static int reply_entry(struct np_drop_fs *fs, np_drop_reply send, void *context, uint64_t unique,
                       uint64_t id, const struct np_drop_attr *attr) {
    struct npf_entry_out entry = {0};
    entry.nodeid = id; entry.generation = 1;
    fill_attr(fs, &entry.attr, id, attr);
    return reply(send, context, unique, 0, &entry, sizeof(entry));
}

/* MARK: Operations */

static int do_init(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                   np_drop_reply send, void *context) {
    (void)fs;
    if (length < sizeof(struct npf_init_in)) return reply(send, context, in->unique, NP_DROP_EINVAL, NULL, 0);
    struct npf_init_in init; memcpy(&init, body, sizeof(init));
    if (init.major < 7) return reply(send, context, in->unique, NP_DROP_EINVAL, NULL, 0);
    struct npf_init_out out = {0};
    out.major = 7;
    /* A newer kernel adapts to our minor; an older one gets its own back. */
    out.minor = init.major > 7 || init.minor > 31 ? 31 : init.minor;
    out.max_readahead = init.max_readahead < NP_DROP_MAX_READ ? init.max_readahead : NP_DROP_MAX_READ;
    out.flags = init.flags & NPF_ASYNC_READ;
    out.max_background = 16; out.congestion_threshold = 12;
    out.max_write = 4096; out.time_gran = 1;
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

static int do_lookup(struct np_drop_fs *fs, const struct npf_in_header *in, const char *name, size_t length,
                     np_drop_reply send, void *context) {
    struct node parent;
    if (read_node(fs, in->nodeid, &parent) < 0) return reply(send, context, in->unique, NP_DROP_ENOENT, NULL, 0);
    struct np_drop_attr attr;
    int error;
    uint64_t id = 0;
    if (parent.id == ROOT_ID) {
        if (!valid_token(name, length)) return reply(send, context, in->unique, NP_DROP_ENOENT, NULL, 0);
        error = host_lookup(fs, name, "", &attr);
        /* Unreachable host: the drop is gone as far as this guest can tell. */
        if (error == NP_DROP_EIO) error = NP_DROP_ENOENT;
        if (!error && !(id = remember(fs, NP_DROP_DIRECTORY, name, ""))) error = NP_DROP_EIO;
    } else if (parent.kind == NP_DROP_DIRECTORY) {
        if (!valid_name(name, length)) return reply(send, context, in->unique, NP_DROP_ENOENT, NULL, 0);
        error = host_lookup(fs, parent.token, name, &attr);
        if (error == NP_DROP_EIO) error = NP_DROP_ENOENT;
        if (!error && !(id = remember(fs, NP_DROP_FILE, parent.token, name))) error = NP_DROP_EIO;
    } else {
        error = NP_DROP_ENOTDIR;
    }
    if (error) return reply(send, context, in->unique, error, NULL, 0);
    return reply_entry(fs, send, context, in->unique, id, &attr);
}

static int do_getattr(struct np_drop_fs *fs, const struct npf_in_header *in, np_drop_reply send, void *context) {
    struct node node;
    if (read_node(fs, in->nodeid, &node) < 0) return reply(send, context, in->unique, NP_DROP_ENOENT, NULL, 0);
    struct np_drop_attr attr = root_attr;
    if (node.id != ROOT_ID) {
        int error = host_lookup(fs, node.token, node.name, &attr);
        if (error) return reply(send, context, in->unique, error, NULL, 0);
    }
    struct npf_attr_out out = {0};
    fill_attr(fs, &out.attr, node.id, &attr);
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

static int do_open(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                   np_drop_reply send, void *context) {
    struct node node;
    if (length < sizeof(struct npf_open_in)) return reply(send, context, in->unique, NP_DROP_EINVAL, NULL, 0);
    if (read_node(fs, in->nodeid, &node) < 0) return reply(send, context, in->unique, NP_DROP_ENOENT, NULL, 0);
    if (node.id == ROOT_ID || node.kind != NP_DROP_FILE) return reply(send, context, in->unique, NP_DROP_EISDIR, NULL, 0);
    struct npf_open_in open; memcpy(&open, body, sizeof(open));
    if ((open.flags & NPF_O_ACCMODE) != 0 || (open.flags & NPF_O_TRUNC))
        return reply(send, context, in->unique, NP_DROP_EROFS, NULL, 0);
    unsigned char request[NP_DROP_TOKEN + NP_DROP_NAME_MAX];
    struct call call = {0};
    if (call_host(fs, NP_DROP_OPEN, request, path_body(request, node.token, node.name), &call) < 0)
        return reply(send, context, in->unique, NP_DROP_EIO, NULL, 0);
    if (call.status) return reply(send, context, in->unique, (int)call.status, NULL, 0);
    if (call.head_length != 8 + NP_DROP_ATTR) return reply(send, context, in->unique, NP_DROP_EIO, NULL, 0);
    /* Without FOPEN_KEEP_CACHE each open drops cached pages: reopening shows
     * the host's current contents. */
    struct npf_open_out out = {0};
    out.fh = np_file_u64(call.head);
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

static int do_read(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                   np_drop_reply send, void *context) {
    if (length < sizeof(struct npf_read_in)) return reply(send, context, in->unique, NP_DROP_EINVAL, NULL, 0);
    struct npf_read_in read; memcpy(&read, body, sizeof(read));
    uint32_t size = read.size < NP_DROP_MAX_READ ? read.size : NP_DROP_MAX_READ;
    unsigned char request[20];
    np_file_put64(request, read.fh); np_file_put64(request + 8, read.offset); np_file_put32(request + 16, size);
    struct call call = {0};
    call.data = malloc(size ? size : 1);
    call.data_capacity = size;
    int result;
    if (!call.data) result = reply(send, context, in->unique, NP_DROP_EIO, NULL, 0);
    else if (call_host(fs, NP_DROP_READ, request, sizeof(request), &call) < 0)
        result = reply(send, context, in->unique, NP_DROP_EIO, NULL, 0);
    else if (call.status) result = reply(send, context, in->unique, (int)call.status, NULL, 0);
    else result = reply(send, context, in->unique, 0, call.data, call.data_length);
    free(call.data);
    return result;
}

static int do_release(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                      np_drop_reply send, void *context) {
    if (length >= sizeof(struct npf_release_in)) {
        struct npf_release_in release; memcpy(&release, body, sizeof(release));
        unsigned char request[8]; np_file_put64(request, release.fh);
        struct call call = {0};
        /* The kernel has already let go; a lost host simply drops the handle. */
        (void)call_host(fs, NP_DROP_RELEASE, request, sizeof(request), &call);
    }
    return reply(send, context, in->unique, 0, NULL, 0);
}

static int do_opendir(struct np_drop_fs *fs, const struct npf_in_header *in, np_drop_reply send, void *context) {
    struct node node;
    if (read_node(fs, in->nodeid, &node) < 0) return reply(send, context, in->unique, NP_DROP_ENOENT, NULL, 0);
    if (node.id != ROOT_ID && node.kind != NP_DROP_DIRECTORY) return reply(send, context, in->unique, NP_DROP_ENOTDIR, NULL, 0);
    struct npf_open_out out = {0};
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

static size_t add_dirent(unsigned char *out, size_t capacity, uint64_t ino, uint64_t offset, uint32_t type,
                         const char *name, size_t length) {
    size_t size = (sizeof(struct npf_dirent) + length + 7) & ~(size_t)7;
    if (size > capacity) return 0;
    struct npf_dirent entry = {ino, offset, (uint32_t)length, type};
    memset(out, 0, size);
    memcpy(out, &entry, sizeof(entry));
    memcpy(out + sizeof(entry), name, length);
    return size;
}

static int do_readdir(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                      np_drop_reply send, void *context) {
    if (length < sizeof(struct npf_read_in)) return reply(send, context, in->unique, NP_DROP_EINVAL, NULL, 0);
    struct npf_read_in read; memcpy(&read, body, sizeof(read));
    struct node node;
    if (read_node(fs, in->nodeid, &node) < 0) return reply(send, context, in->unique, NP_DROP_ENOENT, NULL, 0);
    struct call call = {0};
    call.data = malloc(LIST_MAX); call.data_capacity = LIST_MAX;
    unsigned char *out = malloc(read.size ? read.size : 1);
    if (!call.data || !out) { free(call.data); free(out); return reply(send, context, in->unique, NP_DROP_EIO, NULL, 0); }
    int root = node.id == ROOT_ID;
    int listed = call_host(fs, NP_DROP_LIST, node.token, root ? 0 : NP_DROP_TOKEN, &call) == 0 && !call.status;
    /* With no host the root is simply empty; a drop's own directory is not. */
    if (!listed && !root) { free(call.data); free(out); return reply(send, context, in->unique, NP_DROP_EIO, NULL, 0); }
    size_t used = 0, position = 0, index = 0;
    const char *dots[2] = {".", ".."};
    for (; index < 2; index++) {
        if (index < read.offset) continue;
        size_t added = add_dirent(out + used, read.size - used, ROOT_ID, index + 1, NPF_DT_DIR, dots[index], strlen(dots[index]));
        if (!added) goto done;
        used += added;
    }
    while (listed && position + 2 <= call.data_length) {
        size_t name_length = (size_t)(call.data[position] | call.data[position + 1] << 8);
        if (position + 2 + name_length + NP_DROP_ATTR > call.data_length) break;
        const char *name = (const char *)call.data + position + 2;
        struct np_drop_attr attr;
        int usable = name_length <= NP_DROP_NAME_MAX &&
                     np_drop_attr_decode(call.data + position + 2 + name_length, NP_DROP_ATTR, &attr) == 0 &&
                     (root ? valid_token(name, name_length) && attr.kind == NP_DROP_DIRECTORY
                           : valid_name(name, name_length) && attr.kind == NP_DROP_FILE);
        position += 2 + name_length + NP_DROP_ATTR;
        if (!usable) continue;
        if (index++ < read.offset) continue;
        /* d_ino is informational here; lookups assign the real node IDs. */
        size_t added = add_dirent(out + used, read.size - used, attr.file_id ? attr.file_id : 2, index,
                                  root ? NPF_DT_DIR : NPF_DT_REG, name, name_length);
        if (!added) break;
        used += added;
    }
done:;
    int result = reply(send, context, in->unique, 0, out, used);
    free(call.data); free(out);
    return result;
}

static int do_statfs(const struct npf_in_header *in, np_drop_reply send, void *context) {
    struct npf_kstatfs out = {0};
    out.bsize = out.frsize = 4096; out.namelen = NP_DROP_NAME_MAX;
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

int np_drop_fs_handle(struct np_drop_fs *fs, const void *request, size_t length,
                      np_drop_reply send, void *context) {
    struct npf_in_header in;
    if (length < sizeof(in)) { errno = EPROTO; return -1; }
    memcpy(&in, request, sizeof(in));
    if (in.len != length) { errno = EPROTO; return -1; }
    const unsigned char *body = (const unsigned char *)request + sizeof(in);
    size_t size = length - sizeof(in);
    switch (in.opcode) {
    case NPF_INIT: return do_init(fs, &in, body, size, send, context);
    case NPF_DESTROY:
        pthread_mutex_lock(&fs->lock); fs->destroyed = 1; pthread_mutex_unlock(&fs->lock);
        return reply(send, context, in.unique, 0, NULL, 0);
    case NPF_LOOKUP: {
        /* The name is NUL-terminated within the request. */
        const char *name = (const char *)body;
        size_t name_length = size ? strnlen(name, size) : 0;
        if (name_length == size) return reply(send, context, in.unique, NP_DROP_EINVAL, NULL, 0);
        return do_lookup(fs, &in, name, name_length, send, context);
    }
    case NPF_FORGET:
        if (size >= sizeof(struct npf_forget_in)) {
            struct npf_forget_in forgotten; memcpy(&forgotten, body, sizeof(forgotten));
            forget(fs, in.nodeid, forgotten.nlookup);
        }
        return 0;
    case NPF_BATCH_FORGET:
        if (size >= sizeof(struct npf_batch_forget_in)) {
            struct npf_batch_forget_in batch; memcpy(&batch, body, sizeof(batch));
            for (uint32_t i = 0; i < batch.count &&
                 sizeof(batch) + (i + 1) * sizeof(struct npf_forget_one) <= size; i++) {
                struct npf_forget_one one;
                memcpy(&one, body + sizeof(batch) + i * sizeof(one), sizeof(one));
                forget(fs, one.nodeid, one.nlookup);
            }
        }
        return 0;
    case NPF_INTERRUPT: return 0;
    case NPF_GETATTR: return do_getattr(fs, &in, send, context);
    case NPF_OPEN: return do_open(fs, &in, body, size, send, context);
    case NPF_READ: return do_read(fs, &in, body, size, send, context);
    case NPF_RELEASE: return do_release(fs, &in, body, size, send, context);
    case NPF_OPENDIR: return do_opendir(fs, &in, send, context);
    case NPF_READDIR: return do_readdir(fs, &in, body, size, send, context);
    case NPF_RELEASEDIR: case NPF_FLUSH: case NPF_FSYNC: case NPF_FSYNCDIR:
        return reply(send, context, in.unique, 0, NULL, 0);
    case NPF_STATFS: return do_statfs(&in, send, context);
    case NPF_SETATTR: case NPF_SYMLINK: case NPF_MKNOD: case NPF_MKDIR: case NPF_UNLINK:
    case NPF_RMDIR: case NPF_RENAME: case NPF_LINK: case NPF_WRITE: case NPF_SETXATTR:
    case NPF_REMOVEXATTR: case NPF_CREATE: case NPF_FALLOCATE: case NPF_RENAME2:
    case NPF_COPY_FILE_RANGE: case NPF_TMPFILE:
        return reply(send, context, in.unique, NP_DROP_EROFS, NULL, 0);
    default:
        /* ENOSYS tells the kernel not to ask again (ACCESS, xattrs, locks...). */
        return reply(send, context, in.unique, NPF_ENOSYS, NULL, 0);
    }
}
