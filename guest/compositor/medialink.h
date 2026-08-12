#ifndef NATIVEPIPE_MEDIALINK_H
#define NATIVEPIPE_MEDIALINK_H

#include "media.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct np_media {
	int listen_fd;
	int conn_fd;
	bool just_attached;
};

bool np_media_listen(struct np_media *media);
void np_media_finish(struct np_media *media);
void np_media_accept(struct np_media *media);
/// Drop a half-closed peer so a new client can attach.
void np_media_pump(struct np_media *media);
bool np_media_connected(const struct np_media *media);

/// Send one NPEN frame. Blocks briefly; drops if no client.
bool np_media_send(struct np_media *media, uint32_t surface_id,
                   uint16_t width, uint16_t height, uint64_t pts_ns,
                   uint16_t epoch, const uint8_t *payload, uint32_t length);

#endif
