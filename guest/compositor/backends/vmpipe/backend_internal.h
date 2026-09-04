#ifndef NP_VMPIPE_BACKEND_INTERNAL_H
#define NP_VMPIPE_BACKEND_INTERNAL_H

#include "compositor_internal.h"
#include "hostlink.h"
#include "vk_context.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct np_vmpipe_backend {
	struct np_vk_context vk;
	bool vk_ready;
	int drm_fd;
	struct np_host host;
	struct np_host host_control;
	struct np_host host_input;
	struct np_host host_feedback;
	struct wl_event_source *host_connection_source;
	struct wl_event_source *host_control_connection_source;
	struct wl_event_source *host_input_connection_source;
	struct wl_event_source *host_feedback_connection_source;
	int watched_host_fd;
	int watched_host_control_fd;
	int watched_host_input_fd;
	int watched_host_feedback_fd;
	uint32_t watched_host_mask;
	uint32_t watched_host_control_mask;
	uint32_t watched_host_input_mask;
	uint32_t watched_host_feedback_mask;
};

static inline struct np_vmpipe_backend *np_vmpipe_backend(
	const struct np_server *server)
{
	return server ? (struct np_vmpipe_backend *)server->backend_state : NULL;
}

#endif
