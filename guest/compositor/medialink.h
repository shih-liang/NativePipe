#ifndef NATIVEPIPE_MEDIALINK_H
#define NATIVEPIPE_MEDIALINK_H

#include "media.h"

#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

struct np_media {
	int listen_fd;
	int conn_fd;
	bool just_attached;
	pthread_mutex_t lock;
	bool lock_initialized;
};

bool np_media_listen(struct np_media *media);
void np_media_finish(struct np_media *media);
void np_media_accept(struct np_media *media);
/// Drop a half-closed peer so a new client can attach.
void np_media_pump(struct np_media *media);
bool np_media_connected(struct np_media *media);
int np_media_connection_fd(struct np_media *media);
void np_media_disconnect(struct np_media *media);
bool np_media_take_just_attached(struct np_media *media);

/// Send one NPEN frame. Blocks briefly; drops if no client.
bool np_media_send(struct np_media *media, uint8_t codec, uint8_t flags,
                   uint32_t surface_id,
                   uint32_t resource_id,
                   uint16_t width, uint16_t height, uint64_t pts_ns,
                   uint16_t epoch, const uint8_t *payload, uint32_t length);

#endif
