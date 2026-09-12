#ifndef NATIVEPIPE_MEDIALINK_H
#define NATIVEPIPE_MEDIALINK_H
#include "media.h"
#include "flow.h"
#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
struct np_media_frame;
/* One bounded writer serializes NPIP and NPEN frames on SSH stdout.
 * Input is serviced independently by the Wayland loop. */
struct np_media {
    int conn_fd, error_fd;
    bool stopping, failed, initialized;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_t thread;
    struct np_media_frame *head[3], *tail[3];
    size_t queued_bytes, display_bytes;
    struct np_remote_flow flow;
    unsigned next_lane;
};
bool np_media_open(struct np_media *media, int fd);
void np_media_finish(struct np_media *media);
bool np_media_connected(struct np_media *media);
bool np_media_can_encode(struct np_media *media);
bool np_media_acknowledge(struct np_media *media, uint32_t bytes);
void np_media_wake(struct np_media *media);
bool np_media_send_binary(struct np_media *media, const void *payload, size_t length);
bool np_media_send_display(struct np_media *media, const void *payload, size_t length);
bool np_media_send(struct np_media *media, uint8_t codec, uint8_t flags,
    uint32_t surface_id, uint32_t resource_id, uint16_t width, uint16_t height,
    uint64_t pts_ns, uint16_t epoch, const uint8_t *payload, uint32_t length);
#endif
