#define _GNU_SOURCE
#include "np_file_rpc.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

uint32_t np_file_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint64_t np_file_u64(const unsigned char *p) {
    return np_file_u32(p) | (uint64_t)np_file_u32(p + 4) << 32;
}
void np_file_put32(unsigned char *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(n >> (8 * i));
}
void np_file_put64(unsigned char *p, uint64_t n) {
    np_file_put32(p, (uint32_t)n); np_file_put32(p + 4, (uint32_t)(n >> 32));
}
static int read_all(int fd, void *bytes, size_t size) {
    unsigned char *p = bytes;
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
        p += n; size -= (size_t)n;
    }
    return 0;
}
static int write_all(int fd, const void *bytes, size_t size, int socket) {
    const unsigned char *p = bytes;
#ifdef SO_NOSIGPIPE
    if (socket) { int yes = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)); }
#endif
    while (size) {
#ifdef MSG_NOSIGNAL
        ssize_t n = socket ? send(fd, p, size, MSG_NOSIGNAL) : write(fd, p, size);
#else
        ssize_t n = write(fd, p, size);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        p += n; size -= (size_t)n;
    }
    return 0;
}
int np_file_send(int socket, uint8_t type, uint16_t flags, uint32_t status,
                 const void *data, size_t length) {
    if (length > NP_FILE_CHUNK) { errno = EMSGSIZE; return -1; }
    unsigned char header[NP_FILE_HEADER] = {'N','P','F','R',NP_FILE_VERSION,type,0,0};
    header[6] = (unsigned char)flags; header[7] = (unsigned char)(flags >> 8);
    np_file_put32(header + 8, (uint32_t)length); np_file_put32(header + 12, status);
    if (write_all(socket, header, sizeof(header), 1) < 0) return -1;
    return write_all(socket, data, length, 1);
}
int np_file_receive(int socket, struct np_file_frame *frame) {
    unsigned char header[NP_FILE_HEADER];
    if (read_all(socket, header, sizeof(header)) < 0) return -1;
    if (memcmp(header, "NPFR", 4) || header[4] != NP_FILE_VERSION ||
        header[5] < NP_FILE_STAT || header[5] > NP_FILE_MKDIR) {
        errno = EPROTO; return -1;
    }
    frame->type = header[5]; frame->flags = header[6] | (uint16_t)header[7] << 8;
    frame->length = np_file_u32(header + 8); frame->status = np_file_u32(header + 12);
    if (frame->length > NP_FILE_CHUNK ||
        (frame->flags && (frame->type != NP_FILE_WRITE || frame->flags != NP_FILE_REPLACE))) {
        errno = EPROTO; return -1;
    }
    return read_all(socket, frame->data, frame->length);
}
static int send_stream(int socket, int source, uint64_t maximum, int framed) {
    unsigned char bytes[NP_FILE_CHUNK];
    uint64_t total = 0;
    for (;;) {
        size_t size = maximum - total < sizeof(bytes) ? (size_t)(maximum - total) : sizeof(bytes);
        ssize_t n;
        do { n = size ? read(source, bytes, size) : 0; } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            if (!framed) {
                if (n < 0) return -1;
                if (total != maximum) { errno = EIO; return -1; }
                return 0;
            }
            unsigned char end[8]; np_file_put64(end, total);
            int error = n < 0 ? errno : 0;
            int result = np_file_send(socket, NP_FILE_END, 0, (uint32_t)error, end, sizeof(end));
            if (error) { errno = error; return -1; }
            return result;
        }
        if ((framed ? np_file_send(socket, NP_FILE_DATA, 0, 0, bytes, (size_t)n)
                    : write_all(socket, bytes, (size_t)n, 1)) < 0) return -1;
        total += (uint64_t)n;
    }
}
int np_file_send_stream(int socket, int source, uint64_t maximum) {
    return send_stream(socket, source, maximum, 1);
}
int np_file_send_bytes(int socket, int source, uint64_t length) {
    return send_stream(socket, source, length, 0);
}
int np_file_receive_stream(int socket, int destination, uint64_t maximum,
                           uint64_t *received) {
    uint64_t total = 0;
    struct np_file_frame frame;
    for (;;) {
        if (np_file_receive(socket, &frame) < 0) return -1;
        if (frame.status) { errno = (int)frame.status; return -1; }
        if (frame.type == NP_FILE_END) {
            if (frame.length != 8 || np_file_u64(frame.data) != total) { errno = EPROTO; return -1; }
            if (received) *received = total;
            return 0;
        }
        if (frame.type != NP_FILE_DATA || !frame.length || frame.length > maximum - total) {
            errno = EPROTO; return -1;
        }
        if (write_all(destination, frame.data, frame.length, 0) < 0) return -1;
        total += frame.length;
    }
}
