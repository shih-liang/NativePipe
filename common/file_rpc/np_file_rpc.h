#ifndef NP_FILE_RPC_H
#define NP_FILE_RPC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* One operation per connected stream. No user IDs or privilege changes on wire.
 * The embedding process supplies its root FD and effective credentials. */
#define NP_FILE_VERSION 1
#define NP_FILE_CHUNK 65536u
#define NP_FILE_HEADER 16u
#define NP_FILE_ROOT_PORT 1025u
#define NP_FILE_USER_PORT 1026u
enum np_file_type {
    NP_FILE_STAT = 1, NP_FILE_LIST, NP_FILE_READ, NP_FILE_WRITE,
    NP_FILE_DATA, NP_FILE_ENTRIES, NP_FILE_END, NP_FILE_METADATA, NP_FILE_MKDIR
};
#define NP_FILE_REPLACE 1u

struct np_file_frame {
    uint8_t type;
    uint16_t flags;
    uint32_t status, length;
    unsigned char data[NP_FILE_CHUNK];
};
/* Large C arrays are not imported as Swift tuples. */
static inline unsigned char *np_file_frame_data(struct np_file_frame *frame) { return frame->data; }

uint32_t np_file_u32(const unsigned char *p);
uint64_t np_file_u64(const unsigned char *p);
void np_file_put32(unsigned char *p, uint32_t n);
void np_file_put64(unsigned char *p, uint64_t n);
int np_file_send(int socket, uint8_t type, uint16_t flags, uint32_t status,
                 const void *data, size_t length);
int np_file_receive(int socket, struct np_file_frame *frame);
/* SOCK_STREAM supplies backpressure without a roundtrip for every chunk.
 * shutdown + close cancels even a writer stalled by backpressure. */
int np_file_send_stream(int socket, int source, uint64_t maximum);
/* Exact-length NPAG payloads let an installed bootstrap fetch its update. */
int np_file_send_bytes(int socket, int source, uint64_t length);
int np_file_receive_stream(int socket, int destination, uint64_t maximum,
                           uint64_t *received);
int np_file_peer_is_host(int socket);
int np_file_listen_vsock(uint32_t port);
int np_file_open(int root_fd, const char *path, int flags, unsigned mode);
int np_file_serve(int socket, int root_fd, int read_only);

/* Optional listener, with bounded concurrent operations. Bootstrap only links
 * the client/wire objects; compositor/guestd/recovery use this same server. */
struct np_file_service;
struct np_file_service *np_file_service_start(uint32_t port, const char *root);
void np_file_service_stop(struct np_file_service *service);
#endif
