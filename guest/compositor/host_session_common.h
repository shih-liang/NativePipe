#ifndef NATIVEPIPE_HOST_SESSION_COMMON_H
#define NATIVEPIPE_HOST_SESSION_COMMON_H

#include "hostlink.h"

#include <wayland-server-core.h>

struct np_server;

/* Transport-independent state replay after a host reconnects. */
void np_host_session_replay_metadata(struct np_server *server);

/* Keep one non-blocking host stream registered with the Wayland event loop. */
void np_host_session_watch(
	struct np_server *server, struct np_host *host,
	struct wl_event_source **source, int *watched_fd, uint32_t *watched_mask,
	wl_event_loop_fd_func_t callback);

#endif
