#include "medialink.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#define NP_MEDIA_MAX_PAYLOAD (32u * 1024u * 1024u)
#define NP_WRITER_LIMIT (2u * (NP_MEDIA_MAX_PAYLOAD + NP_MEDIA_HEADER_SIZE))
struct np_media_frame {
    struct np_media_frame *next;
    size_t size;
    unsigned char bytes[];
};

/* Called with the queue lock held, including from the encoder thread. */
static void fail_writer(struct np_media *m)
{
    m->failed = true;
    uint64_t wake = 1;
    (void)write(m->error_fd, &wake, sizeof(wake));
    pthread_cond_signal(&m->ready);
}

static bool write_frame(struct np_media *m, struct np_media_frame *f)
{
    size_t offset = 0;
    while (offset < f->size) {
        pthread_mutex_lock(&m->lock);
        bool stop = m->stopping;
        pthread_mutex_unlock(&m->lock);
        if (stop) return false;
        ssize_t n = write(m->conn_fd, f->bytes + offset, f->size - offset);
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
static void *writer(void *data)
{
    struct np_media *m = data;
    pthread_mutex_lock(&m->lock);
    while (!m->stopping && !m->failed) {
        while (!m->stopping && !m->failed && !m->head) pthread_cond_wait(&m->ready, &m->lock);
        if (m->stopping || m->failed) break;
        struct np_media_frame *f = m->head;
        m->head = f->next;
        if (!m->head) m->tail = NULL;
        pthread_mutex_unlock(&m->lock);
        bool ok = write_frame(m, f);
        pthread_mutex_lock(&m->lock);
        m->queued_bytes -= f->size;
        free(f);
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
    while (m->head) {
        struct np_media_frame *next = m->head->next;
        free(m->head); m->head = next;
    }
    close(m->conn_fd);
    close(m->error_fd);
    pthread_cond_destroy(&m->ready);
    pthread_mutex_destroy(&m->lock);
    m->initialized = false;
}
static bool enqueue(struct np_media *m, const void *header, size_t header_size,
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
    f->next = NULL; f->size = size;
    memcpy(f->bytes, header, header_size);
    memcpy(f->bytes + header_size, payload, length);
    if (m->tail) m->tail->next = f; else m->head = f;
    m->tail = f;
    m->queued_bytes += size; /* Includes the in-flight frame. */
    pthread_cond_signal(&m->ready);
    pthread_mutex_unlock(&m->lock);
    return true;
}
bool np_media_send_binary(struct np_media *m, const void *payload, size_t length)
{
    if (length > 8u * 1024u * 1024u) return false;
    unsigned char h[12] = {'N','P','I','P',1,0,0,0,
        length, length >> 8, length >> 16, length >> 24};
    return enqueue(m, h, sizeof(h), payload, length);
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
    return enqueue(m, h, sizeof(h), payload, length);
}
