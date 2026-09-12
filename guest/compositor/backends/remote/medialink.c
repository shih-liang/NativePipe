#include "medialink.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <time.h>

#define NP_MEDIA_MAX_PAYLOAD (32u * 1024u * 1024u)
#define NP_WRITER_LIMIT (2u * (NP_MEDIA_MAX_PAYLOAD + NP_MEDIA_HEADER_SIZE))
#define NP_FRAGMENT_SIZE (16u * 1024u)
struct np_media_frame {
    struct np_media_frame *next;
    size_t size, offset;
    unsigned char bytes[];
};

void np_media_wake(struct np_media *m)
{
    uint64_t wake = 1;
    (void)write(m->error_fd, &wake, sizeof(wake));
}

/* Called with the queue lock held, including from the encoder thread. */
static void fail_writer(struct np_media *m)
{
    m->failed = true;
    np_media_wake(m);
    pthread_cond_signal(&m->ready);
}

static bool write_bytes(struct np_media *m, const unsigned char *bytes, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        pthread_mutex_lock(&m->lock);
        bool stop = m->stopping;
        pthread_mutex_unlock(&m->lock);
        if (stop) return false;
        ssize_t n = write(m->conn_fd, bytes + offset, size - offset);
        if (n > 0) { offset += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { .fd = m->conn_fd, .events = POLLOUT };
            int ready = poll(&p, 1, 50);
            if (ready < 0 && errno == EINTR) continue;
            if (ready >= 0 && !(p.revents & (POLLERR | POLLHUP | POLLNVAL)))
                continue;
        }
        return false;
    }
    return true;
}
static void put32(unsigned char *p, uint32_t value) { memcpy(p, &value, 4); }
static double now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int ready_lane(struct np_media *m)
{
    if (m->head[0]) return 0;
    for (unsigned i = 0; i < 2; i++) {
        /* Give interactive pixels three turns, then one background turn. */
        unsigned preferred = m->next_lane == 3 ? 2 : 1;
        unsigned lane = i ? 3 - preferred : preferred;
        struct np_media_frame *f = m->head[lane];
        if (!f) continue;
        if (np_flow_allow(&m->flow, f->size - f->offset)) return (int)lane;
    }
    return -1;
}
static void *writer(void *data)
{
    struct np_media *m = data;
    pthread_mutex_lock(&m->lock);
    while (!m->stopping && !m->failed) {
        int lane;
        while (!m->stopping && !m->failed && (lane = ready_lane(m)) < 0)
            pthread_cond_wait(&m->ready, &m->lock);
        if (m->stopping || m->failed) break;
        struct np_media_frame *f = m->head[lane];
        size_t count = f->size - f->offset;
        unsigned char chunk[28 + NP_FRAGMENT_SIZE];
        const unsigned char *bytes = f->bytes;
        size_t size = count;
        if (lane) {
            count = np_flow_allow(&m->flow, count);
            memcpy(chunk, "NPIP\1\0\0\0", 8);
            put32(chunk + 8, (uint32_t)(16 + count));
            memcpy(chunk + 12, "NPRF", 4);
            put32(chunk + 16, (uint32_t)lane);
            put32(chunk + 20, (uint32_t)f->size);
            put32(chunk + 24, (uint32_t)f->offset);
            memcpy(chunk + 28, f->bytes + f->offset, count);
            bytes = chunk; size = 28 + count;
            np_flow_sent(&m->flow, count, now_seconds());
            m->next_lane = (m->next_lane + 1) % 4;
        }
        pthread_mutex_unlock(&m->lock);
        bool ok = write_bytes(m, bytes, size);
        pthread_mutex_lock(&m->lock);
        f->offset += count;
        m->queued_bytes -= count;
        if (lane == 1) m->display_bytes -= count;
        if (f->offset == f->size) {
            m->head[lane] = f->next;
            if (!m->head[lane]) m->tail[lane] = NULL;
            free(f);
        }
        np_media_wake(m);
        if (!ok) {
            fail_writer(m);
            break;
        }
    }
    pthread_mutex_unlock(&m->lock);
    return NULL;
}
bool np_media_open(struct np_media *m, int fd)
{
    memset(m, 0, sizeof(*m));
    m->conn_fd = fd;
    m->error_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m->error_fd < 0) { close(fd); return false; }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto fail;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (pthread_mutex_init(&m->lock, NULL)) goto fail;
    if (pthread_cond_init(&m->ready, NULL)) {
        pthread_mutex_destroy(&m->lock); goto fail;
    }
    if (pthread_create(&m->thread, NULL, writer, m)) {
        pthread_cond_destroy(&m->ready);
        pthread_mutex_destroy(&m->lock);
        goto fail;
    }
    m->initialized = true;
    return true;
fail:
    close(m->error_fd);
    close(fd);
    return false;
}
bool np_media_connected(struct np_media *m)
{
    if (!m->initialized) return false;
    pthread_mutex_lock(&m->lock);
    bool ok = !m->failed && !m->stopping;
    pthread_mutex_unlock(&m->lock);
    return ok;
}
void np_media_finish(struct np_media *m)
{
    if (!m->initialized) return;
    pthread_mutex_lock(&m->lock);
    m->stopping = true;
    pthread_cond_signal(&m->ready);
    pthread_mutex_unlock(&m->lock);
    pthread_join(m->thread, NULL);
    for (unsigned lane = 0; lane < 3; lane++) {
        while (m->head[lane]) {
            struct np_media_frame *next = m->head[lane]->next;
            free(m->head[lane]); m->head[lane] = next;
        }
    }
    close(m->conn_fd);
    close(m->error_fd);
    pthread_cond_destroy(&m->ready);
    pthread_mutex_destroy(&m->lock);
    m->initialized = false;
}
static bool enqueue(struct np_media *m, unsigned lane, const void *header, size_t header_size,
                    const void *payload, size_t length)
{
    if (!m->initialized || !payload || !length) return false;
    size_t size = header_size + length;
    pthread_mutex_lock(&m->lock);
    if (m->failed || m->stopping || size > NP_WRITER_LIMIT - m->queued_bytes) {
        if (!m->stopping) fail_writer(m);
        pthread_mutex_unlock(&m->lock);
        return false;
    }
    struct np_media_frame *f = malloc(sizeof(*f) + size);
    if (!f) {
        fail_writer(m);
        pthread_mutex_unlock(&m->lock);
        return false;
    }
    f->next = NULL; f->size = size; f->offset = 0;
    memcpy(f->bytes, header, header_size);
    memcpy(f->bytes + header_size, payload, length);
    if (m->tail[lane]) m->tail[lane]->next = f; else m->head[lane] = f;
    m->tail[lane] = f;
    m->queued_bytes += size; /* Bytes still owned by the writer's queues. */
    if (lane == 1) m->display_bytes += size;
    pthread_cond_signal(&m->ready);
    pthread_mutex_unlock(&m->lock);
    return true;
}
static bool send_binary(struct np_media *m, unsigned lane, const void *payload, size_t length)
{
    if (length > 8u * 1024u * 1024u) return false;
    unsigned char h[12] = {'N','P','I','P',1,0,0,0,
        length, length >> 8, length >> 16, length >> 24};
    return enqueue(m, lane, h, sizeof(h), payload, length);
}
bool np_media_send_binary(struct np_media *m, const void *payload, size_t length)
{
    const unsigned char *p = payload;
    /* A catalog end marker must follow every batch, even though it is short.
     * Other replies have independent request tokens and may bypass the catalog. */
    if (length >= 12 && !memcmp(p, "NPAP", 4) && (p[5] == 1 || p[5] == 4))
        return send_binary(m, 2, payload, length);
    return send_binary(m, length > 1024 ? 2 : 0, payload, length);
}
bool np_media_send_display(struct np_media *m, const void *payload, size_t length)
{
    return send_binary(m, 1, payload, length);
}
bool np_media_can_encode(struct np_media *m)
{
    pthread_mutex_lock(&m->lock);
    bool ready = !m->failed && !m->stopping &&
        m->display_bytes < (m->flow.window ? m->flow.window : 65536);
    pthread_mutex_unlock(&m->lock);
    return ready;
}
bool np_media_acknowledge(struct np_media *m, uint32_t count)
{
    pthread_mutex_lock(&m->lock);
    bool valid = np_flow_ack(&m->flow, count, now_seconds());
    if (!valid) fail_writer(m);
    pthread_cond_signal(&m->ready);
    pthread_mutex_unlock(&m->lock);
    np_media_wake(m);
    return valid;
}
bool np_media_send(struct np_media *m, uint8_t codec, uint8_t flags,
    uint32_t surface_id, uint32_t resource_id, uint16_t width, uint16_t height,
    uint64_t pts_ns, uint16_t epoch, const uint8_t *payload, uint32_t length)
{
    if (length > NP_MEDIA_MAX_PAYLOAD) return false;
    unsigned char h[NP_MEDIA_HEADER_SIZE] = {0};
    memcpy(h, NP_MEDIA_MAGIC, 4);
    h[4] = NP_MEDIA_VERSION; h[5] = codec; h[6] = flags;
    /* Supported remote architectures aarch64 and x86_64 are little-endian. */
    memcpy(h + 8, &surface_id, 4); memcpy(h + 12, &resource_id, 4);
    memcpy(h + 16, &width, 2); memcpy(h + 18, &height, 2);
    memcpy(h + 20, &pts_ns, 8); memcpy(h + 28, &length, 4);
    memcpy(h + 32, &epoch, 2);
    return enqueue(m, 1, h, sizeof(h), payload, length);
}
