#define _GNU_SOURCE
#include "drops.h"
#include "np_drop_fs.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Bytes the session may have waiting in the SSH writer before the pump waits:
 * the bulk lane's own credit already paces them onto the wire. */
#define PUMP_QUEUE (1024u * 1024u)
#define PUMP_CHUNK (60u * 1024u)

static void *pump(void *argument) {
    struct np_remote_drops *drops = argument;
    static unsigned char buffer[4 + PUMP_CHUNK];
    memcpy(buffer, "NPFS", 4);
    for (;;) {
        ssize_t count = read(drops->fd, buffer + 4, PUMP_CHUNK);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        while (np_media_queued_bytes(drops->media) > PUMP_QUEUE && np_media_connected(drops->media)) usleep(2000);
        /* One ordered lane: session bytes must not overtake each other. */
        if (!np_media_send_background(drops->media, buffer, 4 + (size_t)count)) break;
    }
    return NULL;
}

bool np_remote_drops_start(struct np_remote_drops *drops, struct np_media *media) {
    memset(drops, 0, sizeof(*drops));
    drops->fd = -1;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) return false;
    drops->service = np_drop_service_start_remote(sockets[0]);
    if (!drops->service) {
        fprintf(stderr, "[drops] %s; dropped files are copied instead\n", strerror(errno));
        close(sockets[1]);
        return false;
    }
    drops->media = media;
    drops->fd = sockets[1];
    if (pthread_create(&drops->pump, NULL, pump, drops) != 0) {
        np_drop_service_stop(drops->service);
        close(drops->fd); drops->fd = -1;
        return false;
    }
    drops->running = true;
    return true;
}

bool np_remote_drops_receive(struct np_remote_drops *drops, const unsigned char *bytes, size_t length) {
    if (!drops->running) return false;
    /* The session's reader drains this socket continuously, and NPFR credit
     * bounds what the host may have in flight, so this write cannot stall. */
    while (length) {
        ssize_t written = send(drops->fd, bytes, length, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        bytes += written; length -= (size_t)written;
    }
    return true;
}

void np_remote_drops_stop(struct np_remote_drops *drops) {
    if (!drops->running) return;
    drops->running = false;
    np_drop_service_stop(drops->service);
    shutdown(drops->fd, SHUT_RDWR);
    pthread_join(drops->pump, NULL);
    close(drops->fd); drops->fd = -1;
}
