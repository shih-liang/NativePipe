#ifndef NP_FILE_RPC_H
#define NP_FILE_RPC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Guest file services over NPFR sessions (np_file_session.h). The guest
 * listens, the host connects, and every request is one stream. No user IDs or
 * privilege changes on the wire: the embedding process supplies its root and
 * effective credentials. See README.md "File services". */
#define NP_FILE_ROOT_PORT 1025u
#define NP_FILE_USER_PORT 1026u
#define NP_FILE_REVISION 48u
#define NP_FILE_METADATA 28u
/* READ, WRITE and RANGE move at most this many bytes per request. */
#define NP_FILE_IO_MAX 1048576u
#define NP_FILE_PATH_MAX 4095u
/* Open handles one session may hold. */
#define NP_FILE_HANDLES 64u
#define NP_FILE_STAGING_TOKEN 32u
/* Stable on-wire error; host and guest errno numbering differs. */
#define NP_FILE_STALE 116u

/* Request opcodes. A path is path_length:u32 followed by its bytes. */
enum np_file_opcode {
    NP_FILE_STAT = 1,         /* path -> metadata */
    NP_FILE_OPEN,             /* flags:u32, mode:u32, path -> handle:u64, metadata[, home] */
    NP_FILE_READ,             /* handle:u64, length:u32 -> DATA (empty at the end) */
    NP_FILE_WRITE,            /* handle:u64; DATA -> written:u32 */
    NP_FILE_CLOSE,            /* handle:u64 -> empty; reports a written file's chmod/fsync/close */
    NP_FILE_MKDIR,            /* flags:u32, path -> empty */
    NP_FILE_SNAPSHOT,         /* path -> metadata, revision */
    NP_FILE_RANGE,            /* offset:u64, length:u32, revision, path -> DATA */
    NP_FILE_CREATE_STAGING,   /* token, path -> empty */
    NP_FILE_PUBLISH_STAGING,  /* token, destination path, path -> empty */
    NP_FILE_DISCARD_STAGING   /* token, path -> empty */
};
/* OPEN and MKDIR flags. */
#define NP_FILE_OPEN_WRITE 1u       /* create a regular file and truncate it */
#define NP_FILE_REPLACE 2u          /* with WRITE: an existing file may be replaced */
#define NP_FILE_NOFOLLOW 4u         /* reject ".", ".." and every symbolic link */
#define NP_FILE_OPEN_DIRECTORY 8u   /* the path must be a directory */
#define NP_FILE_OPEN_STAT 16u       /* directory entries carry lstat metadata */
#define NP_FILE_OPEN_HOME 32u       /* an empty path is the worker's home; reply adds path and home */

uint32_t np_file_u32(const unsigned char *p);
uint64_t np_file_u64(const unsigned char *p);
void np_file_put32(unsigned char *p, uint32_t n);
void np_file_put64(unsigned char *p, uint64_t n);
int np_file_peer_is_host(int socket);
int np_file_listen_vsock(uint32_t port);
/* Resolves an absolute path beneath root_fd (RESOLVE_IN_ROOT). */
int np_file_open(int root_fd, const char *path, int flags, unsigned mode);
/* As np_file_open, but no symbolic link anywhere and no "." or "..". */
int np_file_open_nofollow(int root_fd, const char *path, int flags, unsigned mode);

/* Serves one connected socket until the host disconnects. Takes ownership of
 * the socket; the caller keeps the root descriptor. */
int np_file_serve(int socket, int root_fd, int read_only);

/* Listener with bounded concurrent sessions. compositor/guestd/recovery use
 * this; the installer links only the path resolution above. */
struct np_file_service;
struct np_file_service *np_file_service_start(uint32_t port, const char *root);
void np_file_service_stop(struct np_file_service *service);
#endif
