#ifndef NP_REMOTE_BACKEND_INTERNAL_H
#define NP_REMOTE_BACKEND_INTERNAL_H

#include "compositor_internal.h"
#include "hostlink.h"
#include "medialink.h"

#include <stdint.h>
#include <wayland-server-core.h>

struct np_remote_backend {
	struct np_host host;
	struct np_media media;
	uint32_t next_media_resource_id;
	struct wl_event_source *host_connection_source;
	struct wl_event_source *media_connection_source;
	struct wl_event_source *remote_pair_timeout_source;
	int watched_host_fd;
	uint32_t watched_host_mask;
	int watched_media_fd;
	uint64_t watched_media_generation;
};

static inline struct np_remote_backend *np_remote_backend(
	const struct np_server *server)
{
	return server ? (struct np_remote_backend *)server->backend_state : NULL;
}

#endif
