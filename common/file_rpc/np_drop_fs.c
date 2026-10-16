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
struct npf_write_in { uint64_t fh, offset; uint32_t size, write_flags; uint64_t lock_owner; uint32_t flags, padding; };
struct npf_write_out { uint32_t size, padding; };
struct npf_setattr_in {
    uint32_t valid, padding;
    uint64_t fh, size, lock_owner, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec, mode, unused4, uid, gid, unused5;
};
struct npf_create_in { uint32_t flags, mode, umask, open_flags; };
struct npf_rename_in { uint64_t newdir; };
struct npf_rename2_in { uint64_t newdir; uint32_t flags, padding; };
struct npf_fsync_in { uint64_t fh; uint32_t fsync_flags, padding; };
struct npf_flush_in { uint64_t fh; uint32_t unused, padding; uint64_t lock_owner; };
struct npf_file_lock { uint64_t start, end; uint32_t type, pid; };
struct npf_lk_in { uint64_t fh, owner; struct npf_file_lock lk; uint32_t lk_flags, padding; };
struct npf_lk_out { struct npf_file_lock lk; };
struct npf_interrupt_in { uint64_t unique; };

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
_Static_assert(sizeof(struct npf_write_in) == sizeof(struct fuse_write_in), "fuse_write_in");
_Static_assert(sizeof(struct npf_write_out) == sizeof(struct fuse_write_out), "fuse_write_out");
_Static_assert(sizeof(struct npf_setattr_in) == sizeof(struct fuse_setattr_in), "fuse_setattr_in");
_Static_assert(sizeof(struct npf_create_in) == sizeof(struct fuse_create_in), "fuse_create_in");
_Static_assert(sizeof(struct npf_rename2_in) == sizeof(struct fuse_rename2_in), "fuse_rename2_in");
_Static_assert(sizeof(struct npf_fsync_in) == sizeof(struct fuse_fsync_in), "fuse_fsync_in");
_Static_assert(sizeof(struct npf_flush_in) == sizeof(struct fuse_flush_in), "fuse_flush_in");
_Static_assert(sizeof(struct npf_lk_in) == sizeof(struct fuse_lk_in), "fuse_lk_in");
_Static_assert(sizeof(struct npf_lk_out) == sizeof(struct fuse_lk_out), "fuse_lk_out");
_Static_assert(FUSE_SETLKW == 33 && FUSE_RENAME2 == 45 && FATTR_MTIME_NOW == 256, "opcodes");
#endif
#endif
_Static_assert(sizeof(struct npf_in_header) == 40, "in header");
_Static_assert(sizeof(struct npf_attr) == 88, "attr");
_Static_assert(sizeof(struct npf_init_out) == 64, "init out");
_Static_assert(sizeof(struct npf_setattr_in) == 88, "setattr in");
_Static_assert(sizeof(struct npf_lk_in) == 48, "lk in");

enum {
    NPF_LOOKUP = 1, NPF_FORGET = 2, NPF_GETATTR = 3, NPF_SETATTR = 4, NPF_READLINK = 5,
    NPF_SYMLINK = 6, NPF_MKNOD = 8, NPF_MKDIR = 9, NPF_UNLINK = 10, NPF_RMDIR = 11,
    NPF_RENAME = 12, NPF_LINK = 13, NPF_OPEN = 14, NPF_READ = 15, NPF_WRITE = 16,
    NPF_STATFS = 17, NPF_RELEASE = 18, NPF_FSYNC = 20, NPF_SETXATTR = 21,
    NPF_REMOVEXATTR = 24, NPF_FLUSH = 25, NPF_INIT = 26, NPF_OPENDIR = 27,
    NPF_READDIR = 28, NPF_RELEASEDIR = 29, NPF_FSYNCDIR = 30, NPF_GETLK = 31,
    NPF_SETLK = 32, NPF_SETLKW = 33, NPF_CREATE = 35,
    NPF_INTERRUPT = 36, NPF_DESTROY = 38, NPF_BATCH_FORGET = 42, NPF_FALLOCATE = 43,
    NPF_RENAME2 = 45, NPF_COPY_FILE_RANGE = 47, NPF_TMPFILE = 51,
};
/* Linux values, independent of the build host's headers. */
#define NPF_ENOSYS 38
#define NPF_ASYNC_READ 1u
#define NPF_POSIX_LOCKS 2u
#define NPF_ATOMIC_O_TRUNC 8u
#define NPF_BIG_WRITES 32u
#define NPF_FLOCK_LOCKS 1024u
#define NPF_LK_FLOCK 1u
#define NPF_S_IFDIR 0040000u
#define NPF_S_IFREG 0100000u
#define NPF_S_IFLNK 0120000u
#define NPF_DT_DIR 4u
#define NPF_DT_REG 8u
#define NPF_DT_LNK 10u
#define ROOT_ID 1u
/* One LIST answer; larger directories are read in further pages. */
#define LIST_MAX (1024u * 1024u)
/* token + path, plus the largest fixed prefix any request puts before them. */
#define PATH_BODY (64u + NP_DROP_TOKEN + NP_DROP_PATH_MAX)

/* A node is a path inside one drop: "" is the drop's own directory, "name"
 * the dropped item, "name/sub" inside a dropped folder. */
struct node {
    struct node *next;
    uint64_t id, lookups;
    uint8_t kind;
    /* Unlinked or renamed over: the kernel may still use the ID, lookups may not. */
    int dead;
    char token[NP_DROP_TOKEN + 1];
    char *path;
};
/* A copy of a node's identity, safe to use after the lock is released. */
struct node_view {
    uint64_t id;
    uint8_t kind;
    int dead;
    char token[NP_DROP_TOKEN + 1];
    char path[NP_DROP_PATH_MAX + 1];
};

/* A request that can wait indefinitely (a blocking lock), so INTERRUPT can
 * cancel its host stream. */
struct waiter {
    struct waiter *next;
    uint64_t unique;
    struct np_file2_stream *stream;
    int interrupted;
};
#define INTERRUPTS 16u

struct np_drop_fs {
    struct waiter *waits;
    /* An INTERRUPT may arrive before its request starts waiting. */
    uint64_t interrupts[INTERRUPTS];
    unsigned next_interrupt;
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
    if (length < NP_DROP_ATTR || in[0] < NP_DROP_DIRECTORY || in[0] > NP_DROP_LINK ||
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
    while (fs->nodes) { struct node *next = fs->nodes->next; free(fs->nodes->path); free(fs->nodes); fs->nodes = next; }
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
 * the protocol (errno EIO; the caller answers EIO, or ENOENT for name lookups)
 * or the request was interrupted (errno EINTR). `upload` follows as DATA. A
 * nonzero `unique` lets INTERRUPT cancel the request. */
static int call_host_full(struct np_drop_fs *fs, uint16_t opcode, const void *body, size_t length,
                          const void *upload, size_t upload_length, int has_upload, uint64_t unique,
                          struct call *call) {
    pthread_mutex_lock(&fs->lock);
    struct np_file2_session *session = fs->session;
    unsigned generation = fs->generation & 1u;
    if (session) fs->users[generation]++;
    pthread_mutex_unlock(&fs->lock);
    if (!session) { errno = ENOTCONN; return -1; }
    int result = -1;
    struct waiter wait = {NULL, unique, NULL, 0};
    struct np_file2_message *message = malloc(sizeof(*message));
    struct np_file2_stream *stream = message ? np_file2_request(session, opcode, body, length, !has_upload) : NULL;
    if (stream && unique) {
        pthread_mutex_lock(&fs->lock);
        wait.stream = stream;
        for (unsigned i = 0; i < INTERRUPTS; i++) if (fs->interrupts[i] == unique) wait.interrupted = 1;
        if (wait.interrupted) np_file2_cancel(stream);
        wait.next = fs->waits; fs->waits = &wait;
        pthread_mutex_unlock(&fs->lock);
    }
    if (stream && has_upload && np_file2_send(stream, upload, upload_length, 1) < 0) {
        /* The response still arrives, or the stream reports why it cannot. */
    }
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
    if (stream && unique) {
        pthread_mutex_lock(&fs->lock);
        struct waiter **link = &fs->waits;
        while (*link != &wait) link = &(*link)->next;
        *link = wait.next;
        pthread_mutex_unlock(&fs->lock);
    }
    np_file2_stream_release(stream);
    free(message);
    pthread_mutex_lock(&fs->lock);
    if (--fs->users[generation] == 0) pthread_cond_broadcast(&fs->idle);
    pthread_mutex_unlock(&fs->lock);
    if (result < 0) errno = wait.interrupted ? EINTR : EIO;
    return result;
}
static int call_host(struct np_drop_fs *fs, uint16_t opcode, const void *body, size_t length, struct call *call) {
    return call_host_full(fs, opcode, body, length, NULL, 0, 0, 0, call);
}

static void interrupt(struct np_drop_fs *fs, uint64_t unique) {
    pthread_mutex_lock(&fs->lock);
    struct waiter *wait = fs->waits;
    while (wait && wait->unique != unique) wait = wait->next;
    if (wait) {
        wait->interrupted = 1;
        np_file2_cancel(wait->stream);
    } else {
        fs->interrupts[fs->next_interrupt++ % INTERRUPTS] = unique;
    }
    pthread_mutex_unlock(&fs->lock);
}

static size_t path_body(unsigned char *body, const char *token, const char *path) {
    size_t length = strlen(path);
    memcpy(body, token, NP_DROP_TOKEN);
    memcpy(body + NP_DROP_TOKEN, path, length);
    return NP_DROP_TOKEN + length;
}

/* Returns a Linux errno (0 on success). */
static int host_lookup(struct np_drop_fs *fs, const char *token, const char *path, struct np_drop_attr *attr) {
    unsigned char body[PATH_BODY];
    struct call call = {0};
    if (call_host(fs, NP_DROP_LOOKUP, body, path_body(body, token, path), &call) < 0) return NP_DROP_EIO;
    if (call.status) return (int)call.status;
    if (np_drop_attr_decode(call.head, call.head_length, attr) < 0) return NP_DROP_EIO;
    if (!path[0] && attr->kind != NP_DROP_DIRECTORY) return NP_DROP_EIO;
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
           !(length == 1 && name[0] == '.') && !(length == 2 && name[0] == '.' && name[1] == '.');
}
/* parent/name, or name when the parent is the drop's own directory. */
static int join(char out[NP_DROP_PATH_MAX + 1], const char *parent, const char *name, size_t length) {
    size_t parent_length = strlen(parent), total = parent_length + (parent_length ? 1 : 0) + length;
    if (total > NP_DROP_PATH_MAX) return NP_DROP_ENAMETOOLONG;
    memcpy(out, parent, parent_length);
    if (parent_length) out[parent_length++] = '/';
    memcpy(out + parent_length, name, length);
    out[total] = 0;
    return 0;
}
static int under(const char *path, const char *prefix) {
    size_t length = strlen(prefix);
    return !strncmp(path, prefix, length) && (path[length] == 0 || path[length] == '/');
}

/* Locked. */
static struct node *find_node(struct np_drop_fs *fs, uint64_t id) {
    for (struct node *node = fs->nodes; node; node = node->next) if (node->id == id) return node;
    return NULL;
}
static int read_node(struct np_drop_fs *fs, uint64_t id, struct node_view *out) {
    if (id == ROOT_ID) { memset(out, 0, sizeof(*out)); out->id = ROOT_ID; return 0; }
    pthread_mutex_lock(&fs->lock);
    struct node *node = find_node(fs, id);
    if (node) {
        out->id = node->id; out->kind = node->kind; out->dead = node->dead;
        strcpy(out->token, node->token); strcpy(out->path, node->path);
    }
    pthread_mutex_unlock(&fs->lock);
    return node ? 0 : -1;
}
static uint64_t remember(struct np_drop_fs *fs, uint8_t kind, const char *token, const char *path) {
    pthread_mutex_lock(&fs->lock);
    struct node *node = fs->nodes;
    while (node && !(!node->dead && !strcmp(node->token, token) && !strcmp(node->path, path))) node = node->next;
    /* The same path may now be another kind of object (replaced on the Mac). */
    if (node && node->kind != kind) { node->dead = 1; node = NULL; }
    if (!node && (node = calloc(1, sizeof(*node)))) {
        node->path = strdup(path);
        if (!node->path) { free(node); node = NULL; }
        else {
            node->id = fs->next_id++; node->kind = kind;
            strcpy(node->token, token);
            node->next = fs->nodes; fs->nodes = node;
        }
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
        if (!node->lookups) { *link = node->next; free(node->path); free(node); }
    }
    pthread_mutex_unlock(&fs->lock);
}
/* After an unlink, rmdir or a rename over a path, no lookup may reach the old
 * node or anything below it. */
static void bury(struct np_drop_fs *fs, const char *token, const char *path) {
    pthread_mutex_lock(&fs->lock);
    for (struct node *node = fs->nodes; node; node = node->next)
        if (!node->dead && !strcmp(node->token, token) && under(node->path, path)) node->dead = 1;
    pthread_mutex_unlock(&fs->lock);
}
/* The kernel keeps a moved inode's ID; it and its descendants answer to the new path. */
static void moved(struct np_drop_fs *fs, const char *token, const char *from, const char *to) {
    size_t from_length = strlen(from), to_length = strlen(to);
    pthread_mutex_lock(&fs->lock);
    for (struct node *node = fs->nodes; node; node = node->next) {
        if (node->dead || strcmp(node->token, token) || !under(node->path, from)) continue;
        size_t rest = strlen(node->path) - from_length;
        char *path = to_length + rest <= NP_DROP_PATH_MAX ? malloc(to_length + rest + 1) : NULL;
        if (!path) { node->dead = 1; continue; }
        memcpy(path, to, to_length);
        memcpy(path + to_length, node->path + from_length, rest + 1);
        free(node->path); node->path = path;
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
    int directory = attr->kind == NP_DROP_DIRECTORY, link = attr->kind == NP_DROP_LINK;
    out->ino = ino;
    out->size = directory ? 0 : attr->size;
    out->blocks = (out->size + 511) / 512;
    out->atime = out->mtime = out->ctime = (uint64_t)attr->mtime_seconds;
    out->atimensec = out->mtimensec = out->ctimensec = attr->mtime_nanoseconds;
    out->mode = (directory ? NPF_S_IFDIR : link ? NPF_S_IFLNK : NPF_S_IFREG) | (link ? 0777u : attr->mode & 07777u);
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
#define REPLY(error) reply(send, context, in->unique, (error), NULL, 0)

/* MARK: Operations */

static int do_init(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                   np_drop_reply send, void *context) {
    (void)fs;
    if (length < sizeof(struct npf_init_in)) return REPLY(NP_DROP_EINVAL);
    struct npf_init_in init; memcpy(&init, body, sizeof(init));
    if (init.major < 7) return REPLY(NP_DROP_EINVAL);
    struct npf_init_out out = {0};
    out.major = 7;
    /* A newer kernel adapts to our minor; an older one gets its own back. */
    out.minor = init.major > 7 || init.minor > 31 ? 31 : init.minor;
    out.max_readahead = init.max_readahead < NP_DROP_MAX_READ ? init.max_readahead : NP_DROP_MAX_READ;
    /* Locks are forwarded so Mac programs and guest programs exclude each other. */
    out.flags = init.flags & (NPF_ASYNC_READ | NPF_POSIX_LOCKS | NPF_ATOMIC_O_TRUNC | NPF_BIG_WRITES | NPF_FLOCK_LOCKS);
    out.max_background = 16; out.congestion_threshold = 12;
    out.max_write = NP_DROP_MAX_WRITE; out.time_gran = 1;
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

/* A directory below the root: a drop's own directory or one inside a folder. */
static int directory_node(struct np_drop_fs *fs, uint64_t id, struct node_view *out) {
    if (read_node(fs, id, out) < 0) return NP_DROP_ENOENT;
    if (out->id == ROOT_ID) return NP_DROP_EPERM;
    if (out->dead) return NP_DROP_ENOENT;
    if (out->kind != NP_DROP_DIRECTORY) return NP_DROP_ENOTDIR;
    return 0;
}
/* parent's path joined with a NUL-terminated name taken from the request. */
static int child_path(struct np_drop_fs *fs, uint64_t parent_id, const unsigned char *body, size_t length,
                      struct node_view *parent, char path[NP_DROP_PATH_MAX + 1], size_t *consumed) {
    const char *name = (const char *)body;
    size_t name_length = length ? strnlen(name, length) : 0;
    if (name_length == length) return NP_DROP_EINVAL;
    if (consumed) *consumed = name_length + 1;
    int error = directory_node(fs, parent_id, parent);
    if (error) return error;
    if (!valid_name(name, name_length)) return NP_DROP_EINVAL;
    return join(path, parent->path, name, name_length);
}

static int do_lookup(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                     np_drop_reply send, void *context) {
    const char *name = (const char *)body;
    size_t name_length = length ? strnlen(name, length) : 0;
    if (name_length == length) return REPLY(NP_DROP_EINVAL);
    struct node_view parent;
    if (read_node(fs, in->nodeid, &parent) < 0) return REPLY(NP_DROP_ENOENT);
    struct np_drop_attr attr;
    int error;
    uint64_t id = 0;
    if (parent.id == ROOT_ID) {
        if (!valid_token(name, name_length)) return REPLY(NP_DROP_ENOENT);
        error = host_lookup(fs, name, "", &attr);
        /* Unreachable host: the drop is gone as far as this guest can tell. */
        if (error == NP_DROP_EIO) error = NP_DROP_ENOENT;
        if (!error && !(id = remember(fs, NP_DROP_DIRECTORY, name, ""))) error = NP_DROP_EIO;
    } else if (parent.dead) {
        error = NP_DROP_ENOENT;
    } else if (parent.kind == NP_DROP_DIRECTORY) {
        char path[NP_DROP_PATH_MAX + 1];
        if (!valid_name(name, name_length)) return REPLY(NP_DROP_ENOENT);
        if ((error = join(path, parent.path, name, name_length))) return REPLY(error);
        error = host_lookup(fs, parent.token, path, &attr);
        if (error == NP_DROP_EIO) error = NP_DROP_ENOENT;
        if (!error && !(id = remember(fs, attr.kind, parent.token, path))) error = NP_DROP_EIO;
    } else {
        error = NP_DROP_ENOTDIR;
    }
    if (error) return REPLY(error);
    return reply_entry(fs, send, context, in->unique, id, &attr);
}

static int host_setattr(struct np_drop_fs *fs, const struct node_view *node, uint32_t valid, uint32_t mode, uint64_t size,
                        int64_t atime, uint32_t atime_ns, int64_t mtime, uint32_t mtime_ns, uint64_t handle,
                        struct np_drop_attr *attr) {
    unsigned char request[PATH_BODY];
    np_file_put32(request, valid); np_file_put32(request + 4, mode); np_file_put64(request + 8, size);
    np_file_put64(request + 16, (uint64_t)atime); np_file_put32(request + 24, atime_ns);
    np_file_put64(request + 28, (uint64_t)mtime); np_file_put32(request + 36, mtime_ns);
    np_file_put64(request + 40, handle);
    size_t length = NP_DROP_SETATTR_SIZE + path_body(request + NP_DROP_SETATTR_SIZE, node->token, node->path);
    struct call call = {0};
    if (call_host(fs, NP_DROP_SETATTR, request, length, &call) < 0) return NP_DROP_EIO;
    if (call.status) return (int)call.status;
    if (np_drop_attr_decode(call.head, call.head_length, attr) < 0) return NP_DROP_EIO;
    return 0;
}

static int do_getattr(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                      np_drop_reply send, void *context) {
    struct node_view node;
    if (read_node(fs, in->nodeid, &node) < 0) return REPLY(NP_DROP_ENOENT);
    struct np_drop_attr attr = root_attr;
    /* fstat on an open file asks the host about that open file, which may no
     * longer be at its path (renamed over, unlinked or replaced on the Mac). */
    uint32_t flags = length >= 8 ? np_file_u32(body) : 0;
    uint64_t handle = length >= 16 ? np_file_u64(body + 8) : 0;
    if (node.id != ROOT_ID && node.kind == NP_DROP_FILE && (flags & 1u)) {
        int error = host_setattr(fs, &node, NP_DROP_SET_HANDLE, 0, 0, 0, 0, 0, 0, handle, &attr);
        if (error) return REPLY(error);
    } else if (node.id != ROOT_ID) {
        if (node.dead) return REPLY(NP_DROP_ENOENT);
        int error = host_lookup(fs, node.token, node.path, &attr);
        if (error) return REPLY(error);
    }
    struct npf_attr_out out = {0};
    fill_attr(fs, &out.attr, node.id, &attr);
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

static int do_open(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                   np_drop_reply send, void *context) {
    struct node_view node;
    if (length < sizeof(struct npf_open_in)) return REPLY(NP_DROP_EINVAL);
    if (read_node(fs, in->nodeid, &node) < 0) return REPLY(NP_DROP_ENOENT);
    if (node.id == ROOT_ID || node.kind == NP_DROP_DIRECTORY) return REPLY(NP_DROP_EISDIR);
    if (node.kind != NP_DROP_FILE) return REPLY(NP_DROP_ELOOP);
    if (node.dead) return REPLY(NP_DROP_ENOENT);
    struct npf_open_in open; memcpy(&open, body, sizeof(open));
    unsigned char request[PATH_BODY];
    np_file_put32(request, open.flags);
    struct call call = {0};
    if (call_host(fs, NP_DROP_OPEN, request, 4 + path_body(request + 4, node.token, node.path), &call) < 0)
        return REPLY(NP_DROP_EIO);
    if (call.status) return REPLY((int)call.status);
    if (call.head_length != 8 + NP_DROP_ATTR) return REPLY(NP_DROP_EIO);
    /* Without FOPEN_KEEP_CACHE each open drops cached pages: reopening shows
     * the host's current contents. */
    struct npf_open_out out = {0};
    out.fh = np_file_u64(call.head);
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

static int do_read(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                   np_drop_reply send, void *context) {
    if (length < sizeof(struct npf_read_in)) return REPLY(NP_DROP_EINVAL);
    struct npf_read_in read; memcpy(&read, body, sizeof(read));
    uint32_t size = read.size < NP_DROP_MAX_READ ? read.size : NP_DROP_MAX_READ;
    unsigned char request[20];
    np_file_put64(request, read.fh); np_file_put64(request + 8, read.offset); np_file_put32(request + 16, size);
    struct call call = {0};
    call.data = malloc(size ? size : 1);
    call.data_capacity = size;
    int result;
    if (!call.data) result = REPLY(NP_DROP_EIO);
    else if (call_host(fs, NP_DROP_READ, request, sizeof(request), &call) < 0) result = REPLY(NP_DROP_EIO);
    else if (call.status) result = REPLY((int)call.status);
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
    return REPLY(0);
}

static int do_opendir(struct np_drop_fs *fs, const struct npf_in_header *in, np_drop_reply send, void *context) {
    struct node_view node;
    if (read_node(fs, in->nodeid, &node) < 0) return REPLY(NP_DROP_ENOENT);
    if (node.id != ROOT_ID && node.kind != NP_DROP_DIRECTORY) return REPLY(NP_DROP_ENOTDIR);
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
    if (length < sizeof(struct npf_read_in)) return REPLY(NP_DROP_EINVAL);
    struct npf_read_in read; memcpy(&read, body, sizeof(read));
    struct node_view node;
    if (read_node(fs, in->nodeid, &node) < 0) return REPLY(NP_DROP_ENOENT);
    if (node.id != ROOT_ID && (node.dead || node.kind != NP_DROP_DIRECTORY)) return REPLY(node.dead ? NP_DROP_ENOENT : NP_DROP_ENOTDIR);
    int root = node.id == ROOT_ID;
    unsigned char *out = malloc(read.size ? read.size : 1);
    struct call call = {0};
    call.data = malloc(LIST_MAX); call.data_capacity = LIST_MAX;
    if (!call.data || !out) { free(call.data); free(out); return REPLY(NP_DROP_EIO); }
    size_t used = 0, index = read.offset;
    const char *dots[2] = {".", ".."};
    for (; index < 2; index++) {
        size_t added = add_dirent(out + used, read.size - used, ROOT_ID, index + 1, NPF_DT_DIR, dots[index], strlen(dots[index]));
        if (!added) goto done;
        used += added;
    }
    /* Offsets are entry positions: the host lists from one, page by page. */
    for (int more = 1; more;) {
        unsigned char request[PATH_BODY];
        np_file_put32(request, (uint32_t)(index - 2));
        size_t request_length = 4 + (root ? 0 : path_body(request + 4, node.token, node.path));
        int listed = call_host(fs, NP_DROP_LIST, request, request_length, &call) == 0 && !call.status;
        /* With no host the root is simply empty; a drop's own directory is not. */
        if (!listed) {
            if (root) break;
            free(call.data); free(out);
            return REPLY(call.status ? (int)call.status : NP_DROP_EIO);
        }
        size_t position = 0, entries = 0;
        while (position + 2 <= call.data_length) {
            size_t name_length = (size_t)(call.data[position] | call.data[position + 1] << 8);
            if (position + 2 + name_length + NP_DROP_ATTR > call.data_length) break;
            const char *name = (const char *)call.data + position + 2;
            struct np_drop_attr attr;
            int usable = np_drop_attr_decode(call.data + position + 2 + name_length, NP_DROP_ATTR, &attr) == 0 &&
                         (root ? valid_token(name, name_length) && attr.kind == NP_DROP_DIRECTORY
                               : valid_name(name, name_length));
            position += 2 + name_length + NP_DROP_ATTR;
            entries++;
            /* Skipped names still take their position, so offsets stay stable. */
            if (usable) {
                uint32_t type = attr.kind == NP_DROP_DIRECTORY ? NPF_DT_DIR : attr.kind == NP_DROP_LINK ? NPF_DT_LNK : NPF_DT_REG;
                /* d_ino is informational here; lookups assign the real node IDs. */
                size_t added = add_dirent(out + used, read.size - used, attr.file_id ? attr.file_id : 2, index + 1,
                                          type, name, name_length);
                if (!added) goto done;
                used += added;
            }
            index++;
        }
        more = entries > 0;
    }
done:;
    int result = reply(send, context, in->unique, 0, out, used);
    free(call.data); free(out);
    return result;
}

static int do_readlink(struct np_drop_fs *fs, const struct npf_in_header *in, np_drop_reply send, void *context) {
    struct node_view node;
    if (read_node(fs, in->nodeid, &node) < 0 || node.dead) return REPLY(NP_DROP_ENOENT);
    if (node.kind != NP_DROP_LINK) return REPLY(NP_DROP_EINVAL);
    unsigned char request[PATH_BODY], target[NP_DROP_PATH_MAX + 1];
    struct call call = {0};
    call.data = target; call.data_capacity = NP_DROP_PATH_MAX;
    if (call_host(fs, NP_DROP_READLINK, request, path_body(request, node.token, node.path), &call) < 0)
        return REPLY(NP_DROP_EIO);
    if (call.status) return REPLY((int)call.status);
    return reply(send, context, in->unique, 0, target, call.data_length);
}

static int do_statfs(const struct npf_in_header *in, np_drop_reply send, void *context) {
    struct npf_kstatfs out = {0};
    out.bsize = out.frsize = 4096; out.namelen = NP_DROP_NAME_MAX;
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

/* MARK: Modifying operations */

/* Valid FUSE_SETATTR bits we pass on; the rest (lock owner, ctime, kill-suid)
 * need no host action. */
#define SETATTR_PASSED (NP_DROP_SET_MODE | NP_DROP_SET_SIZE | NP_DROP_SET_ATIME | NP_DROP_SET_MTIME | \
                        NP_DROP_SET_HANDLE | NP_DROP_SET_ATIME_NOW | NP_DROP_SET_MTIME_NOW)

static int do_setattr(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                      np_drop_reply send, void *context) {
    if (length < sizeof(struct npf_setattr_in)) return REPLY(NP_DROP_EINVAL);
    struct npf_setattr_in set; memcpy(&set, body, sizeof(set));
    struct node_view node;
    if (read_node(fs, in->nodeid, &node) < 0) return REPLY(NP_DROP_ENOENT);
    /* The root and each drop's own directory are not the Mac's to change. */
    if (node.id == ROOT_ID || !node.path[0]) return REPLY(NP_DROP_EPERM);
    /* Ownership is the mount's; changing it to anything else is refused. */
    if (((set.valid & NP_DROP_SET_UID) && set.uid != (uint32_t)fs->owner) ||
        ((set.valid & NP_DROP_SET_GID) && set.gid != (uint32_t)fs->group))
        return REPLY(NP_DROP_EPERM);
    uint32_t valid = set.valid & SETATTR_PASSED;
    if (node.kind != NP_DROP_FILE) valid &= ~(uint32_t)NP_DROP_SET_HANDLE;
    if (node.dead && !(valid & NP_DROP_SET_HANDLE)) return REPLY(NP_DROP_ENOENT);
    struct np_drop_attr attr;
    int error = host_setattr(fs, &node, valid, set.mode & 07777u, set.size, (int64_t)set.atime, set.atimensec,
                             (int64_t)set.mtime, set.mtimensec, set.fh, &attr);
    if (error) return REPLY(error);
    struct npf_attr_out out = {0};
    fill_attr(fs, &out.attr, node.id, &attr);
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

static int do_write(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                    np_drop_reply send, void *context) {
    if (length < sizeof(struct npf_write_in)) return REPLY(NP_DROP_EINVAL);
    struct npf_write_in write; memcpy(&write, body, sizeof(write));
    if (write.size > length - sizeof(write) || write.size > NP_DROP_READ_MAX) return REPLY(NP_DROP_EINVAL);
    unsigned char request[16];
    np_file_put64(request, write.fh); np_file_put64(request + 8, write.offset);
    struct call call = {0};
    if (call_host_full(fs, NP_DROP_WRITE, request, sizeof(request), body + sizeof(write), write.size, 1, 0, &call) < 0)
        return REPLY(NP_DROP_EIO);
    if (call.status) return REPLY((int)call.status);
    if (call.head_length != 4 || np_file_u32(call.head) > write.size) return REPLY(NP_DROP_EIO);
    struct npf_write_out out = {np_file_u32(call.head), 0};
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

/* Remembers the node a creating request made and answers with its entry. */
static int created(struct np_drop_fs *fs, const struct npf_in_header *in, const struct node_view *parent,
                   const char *path, const struct call *call, np_drop_reply send, void *context) {
    struct np_drop_attr attr;
    if (np_drop_attr_decode(call->head, call->head_length, &attr) < 0) return REPLY(NP_DROP_EIO);
    uint64_t id = remember(fs, attr.kind, parent->token, path);
    if (!id) return REPLY(NP_DROP_EIO);
    return reply_entry(fs, send, context, in->unique, id, &attr);
}

static int do_create(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                     np_drop_reply send, void *context) {
    if (length <= sizeof(struct npf_create_in)) return REPLY(NP_DROP_EINVAL);
    struct npf_create_in create; memcpy(&create, body, sizeof(create));
    struct node_view parent;
    char path[NP_DROP_PATH_MAX + 1];
    int error = child_path(fs, in->nodeid, body + sizeof(create), length - sizeof(create), &parent, path, NULL);
    if (error) return REPLY(error);
    unsigned char request[PATH_BODY];
    np_file_put32(request, create.flags);
    np_file_put32(request + 4, create.mode & ~create.umask & 07777u);
    struct call call = {0};
    if (call_host(fs, NP_DROP_CREATE, request, 8 + path_body(request + 8, parent.token, path), &call) < 0)
        return REPLY(NP_DROP_EIO);
    if (call.status) return REPLY((int)call.status);
    struct np_drop_attr attr;
    if (call.head_length != 8 + NP_DROP_ATTR || np_drop_attr_decode(call.head + 8, NP_DROP_ATTR, &attr) < 0)
        return REPLY(NP_DROP_EIO);
    uint64_t handle = np_file_u64(call.head);
    uint64_t id = remember(fs, NP_DROP_FILE, parent.token, path);
    if (!id) {
        unsigned char release[8]; np_file_put64(release, handle);
        struct call ignored = {0};
        (void)call_host(fs, NP_DROP_RELEASE, release, sizeof(release), &ignored);
        return REPLY(NP_DROP_EIO);
    }
    /* CREATE answers with an entry followed by the open file. */
    struct { struct npf_entry_out entry; struct npf_open_out open; } out;
    memset(&out, 0, sizeof(out));
    out.entry.nodeid = id; out.entry.generation = 1;
    fill_attr(fs, &out.entry.attr, id, &attr);
    out.open.fh = handle;
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

static int do_mkdir(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                    np_drop_reply send, void *context) {
    if (length <= 8) return REPLY(NP_DROP_EINVAL);
    uint32_t mode = np_file_u32(body), umask = np_file_u32(body + 4);
    struct node_view parent;
    char path[NP_DROP_PATH_MAX + 1];
    int error = child_path(fs, in->nodeid, body + 8, length - 8, &parent, path, NULL);
    if (error) return REPLY(error);
    unsigned char request[PATH_BODY];
    np_file_put32(request, mode & ~umask & 07777u);
    struct call call = {0};
    if (call_host(fs, NP_DROP_MKDIR, request, 4 + path_body(request + 4, parent.token, path), &call) < 0)
        return REPLY(NP_DROP_EIO);
    if (call.status) return REPLY((int)call.status);
    return created(fs, in, &parent, path, &call, send, context);
}

static int do_symlink(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                      np_drop_reply send, void *context) {
    struct node_view parent;
    char path[NP_DROP_PATH_MAX + 1];
    size_t consumed = 0;
    int error = child_path(fs, in->nodeid, body, length, &parent, path, &consumed);
    if (error) return REPLY(error);
    const char *target = (const char *)body + consumed;
    size_t target_length = strnlen(target, length - consumed);
    if (target_length == length - consumed || !target_length || target_length > NP_DROP_PATH_MAX)
        return REPLY(NP_DROP_EINVAL);
    unsigned char request[2 + NP_DROP_PATH_MAX + PATH_BODY];
    request[0] = (unsigned char)target_length; request[1] = (unsigned char)(target_length >> 8);
    memcpy(request + 2, target, target_length);
    struct call call = {0};
    if (call_host(fs, NP_DROP_SYMLINK, request, 2 + target_length + path_body(request + 2 + target_length, parent.token, path), &call) < 0)
        return REPLY(NP_DROP_EIO);
    if (call.status) return REPLY((int)call.status);
    return created(fs, in, &parent, path, &call, send, context);
}

static int do_remove(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                     uint16_t opcode, np_drop_reply send, void *context) {
    struct node_view parent;
    char path[NP_DROP_PATH_MAX + 1];
    int error = child_path(fs, in->nodeid, body, length, &parent, path, NULL);
    if (error) return REPLY(error == NP_DROP_EINVAL ? NP_DROP_ENOENT : error);
    unsigned char request[PATH_BODY];
    struct call call = {0};
    if (call_host(fs, opcode, request, path_body(request, parent.token, path), &call) < 0) return REPLY(NP_DROP_EIO);
    if (!call.status) bury(fs, parent.token, path);
    return REPLY((int)call.status);
}

static int do_rename(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                     int with_flags, np_drop_reply send, void *context) {
    size_t fixed = with_flags ? sizeof(struct npf_rename2_in) : sizeof(struct npf_rename_in);
    if (length <= fixed) return REPLY(NP_DROP_EINVAL);
    uint64_t newdir = np_file_u64(body);
    uint32_t flags = with_flags ? np_file_u32(body + 8) : 0;
    struct node_view parent, target;
    char old_path[NP_DROP_PATH_MAX + 1], new_path[NP_DROP_PATH_MAX + 1];
    size_t consumed = 0;
    int error = child_path(fs, in->nodeid, body + fixed, length - fixed, &parent, old_path, &consumed);
    if (!error) error = child_path(fs, newdir, body + fixed + consumed, length - fixed - consumed, &target, new_path, NULL);
    if (error) return REPLY(error);
    /* A drop's files can only move within that drop. */
    if (strcmp(parent.token, target.token)) return REPLY(NP_DROP_EXDEV);
    if (flags & ~(uint32_t)NP_DROP_RENAME_NOREPLACE) return REPLY(NP_DROP_EINVAL);
    if (under(new_path, old_path) && strcmp(new_path, old_path)) return REPLY(NP_DROP_EINVAL);
    size_t old_length = strlen(old_path), new_length = strlen(new_path);
    unsigned char request[6 + NP_DROP_TOKEN + 2 * NP_DROP_PATH_MAX];
    np_file_put32(request, flags);
    request[4] = (unsigned char)old_length; request[5] = (unsigned char)(old_length >> 8);
    memcpy(request + 6, parent.token, NP_DROP_TOKEN);
    memcpy(request + 6 + NP_DROP_TOKEN, old_path, old_length);
    memcpy(request + 6 + NP_DROP_TOKEN + old_length, new_path, new_length);
    struct call call = {0};
    if (call_host(fs, NP_DROP_RENAME, request, 6 + NP_DROP_TOKEN + old_length + new_length, &call) < 0)
        return REPLY(NP_DROP_EIO);
    if (!call.status && strcmp(old_path, new_path)) {
        bury(fs, parent.token, new_path);
        moved(fs, parent.token, old_path, new_path);
    }
    return REPLY((int)call.status);
}

static int handle_call(struct np_drop_fs *fs, const struct npf_in_header *in, uint16_t opcode,
                       const unsigned char *request, size_t length, np_drop_reply send, void *context) {
    struct call call = {0};
    if (call_host(fs, opcode, request, length, &call) < 0) return REPLY(NP_DROP_EIO);
    return REPLY((int)call.status);
}

static int do_fsync(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                    np_drop_reply send, void *context) {
    if (length < sizeof(struct npf_fsync_in)) return REPLY(NP_DROP_EINVAL);
    unsigned char request[8]; memcpy(request, body, 8);
    return handle_call(fs, in, NP_DROP_FSYNC, request, sizeof(request), send, context);
}

/* Every close(2) of the file flushes; POSIX locks of that owner end there. */
static int do_flush(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                    np_drop_reply send, void *context) {
    if (length < sizeof(struct npf_flush_in)) return REPLY(NP_DROP_EINVAL);
    struct npf_flush_in flush; memcpy(&flush, body, sizeof(flush));
    unsigned char request[16];
    np_file_put64(request, flush.fh); np_file_put64(request + 8, flush.lock_owner);
    struct call call = {0};
    (void)call_host(fs, NP_DROP_FLUSH, request, sizeof(request), &call);
    return REPLY(0);
}

static int do_lock(struct np_drop_fs *fs, const struct npf_in_header *in, const unsigned char *body, size_t length,
                   np_drop_reply send, void *context) {
    if (length < sizeof(struct npf_lk_in)) return REPLY(NP_DROP_EINVAL);
    struct npf_lk_in lock; memcpy(&lock, body, sizeof(lock));
    if (lock.lk.type > 2 || lock.lk.start > lock.lk.end) return REPLY(NP_DROP_EINVAL);
    unsigned char request[NP_DROP_LOCK_SIZE];
    np_file_put64(request, lock.fh); np_file_put64(request + 8, lock.owner);
    np_file_put64(request + 16, lock.lk.start); np_file_put64(request + 24, lock.lk.end);
    np_file_put32(request + 32, lock.lk.type);
    uint32_t flags = (lock.lk_flags & NPF_LK_FLOCK ? NP_DROP_LOCK_FLOCK : 0) |
                     (in->opcode == NPF_SETLKW ? NP_DROP_LOCK_WAIT : 0);
    np_file_put32(request + 36, flags);
    struct call call = {0};
    uint16_t opcode = in->opcode == NPF_GETLK ? NP_DROP_GETLK : NP_DROP_SETLK;
    /* Only a waiting request can be interrupted; the others answer at once. */
    if (call_host_full(fs, opcode, request, sizeof(request), NULL, 0, 0,
                       in->opcode == NPF_SETLKW ? in->unique : 0, &call) < 0)
        return REPLY(errno == EINTR ? NP_DROP_EINTR : NP_DROP_EIO);
    if (call.status || in->opcode != NPF_GETLK) return REPLY((int)call.status);
    if (call.head_length != 24) return REPLY(NP_DROP_EIO);
    struct npf_lk_out out = {{np_file_u64(call.head), np_file_u64(call.head + 8),
                              np_file_u32(call.head + 16), np_file_u32(call.head + 20)}};
    return reply(send, context, in->unique, 0, &out, sizeof(out));
}

int np_drop_fs_handle(struct np_drop_fs *fs, const void *request, size_t length,
                      np_drop_reply send, void *context) {
    struct npf_in_header in_header;
    if (length < sizeof(in_header)) { errno = EPROTO; return -1; }
    memcpy(&in_header, request, sizeof(in_header));
    if (in_header.len != length) { errno = EPROTO; return -1; }
    const struct npf_in_header *in = &in_header;
    const unsigned char *body = (const unsigned char *)request + sizeof(in_header);
    size_t size = length - sizeof(in_header);
    switch (in->opcode) {
    case NPF_INIT: return do_init(fs, in, body, size, send, context);
    case NPF_DESTROY:
        pthread_mutex_lock(&fs->lock); fs->destroyed = 1; pthread_mutex_unlock(&fs->lock);
        return REPLY(0);
    case NPF_LOOKUP: return do_lookup(fs, in, body, size, send, context);
    case NPF_FORGET:
        if (size >= sizeof(struct npf_forget_in)) {
            struct npf_forget_in forgotten; memcpy(&forgotten, body, sizeof(forgotten));
            forget(fs, in->nodeid, forgotten.nlookup);
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
    case NPF_INTERRUPT:
        if (size >= sizeof(struct npf_interrupt_in)) interrupt(fs, np_file_u64(body));
        return 0;
    case NPF_GETATTR: return do_getattr(fs, in, body, size, send, context);
    case NPF_SETATTR: return do_setattr(fs, in, body, size, send, context);
    case NPF_READLINK: return do_readlink(fs, in, send, context);
    case NPF_WRITE: return do_write(fs, in, body, size, send, context);
    case NPF_CREATE: return do_create(fs, in, body, size, send, context);
    case NPF_MKDIR: return do_mkdir(fs, in, body, size, send, context);
    case NPF_SYMLINK: return do_symlink(fs, in, body, size, send, context);
    case NPF_UNLINK: return do_remove(fs, in, body, size, NP_DROP_UNLINK, send, context);
    case NPF_RMDIR: return do_remove(fs, in, body, size, NP_DROP_RMDIR, send, context);
    case NPF_RENAME: return do_rename(fs, in, body, size, 0, send, context);
    case NPF_RENAME2: return do_rename(fs, in, body, size, 1, send, context);
    case NPF_FSYNC: return do_fsync(fs, in, body, size, send, context);
    case NPF_FLUSH: return do_flush(fs, in, body, size, send, context);
    case NPF_GETLK: case NPF_SETLK: case NPF_SETLKW: return do_lock(fs, in, body, size, send, context);
    case NPF_OPEN: return do_open(fs, in, body, size, send, context);
    case NPF_READ: return do_read(fs, in, body, size, send, context);
    case NPF_RELEASE: return do_release(fs, in, body, size, send, context);
    case NPF_OPENDIR: return do_opendir(fs, in, send, context);
    case NPF_READDIR: return do_readdir(fs, in, body, size, send, context);
    case NPF_RELEASEDIR: case NPF_FSYNCDIR: return REPLY(0);
    case NPF_STATFS: return do_statfs(in, send, context);
    /* Plain files, folders and symbolic links only. */
    case NPF_MKNOD: case NPF_LINK: case NPF_TMPFILE: return REPLY(NP_DROP_EPERM);
    case NPF_FALLOCATE: return REPLY(NP_DROP_EOPNOTSUPP);
    default:
        /* ENOSYS tells the kernel not to ask again (ACCESS, xattrs...). */
        return REPLY(NPF_ENOSYS);
    }
}
