#ifndef NP_VMPIPE_SHM_TEXTURE_INTERNAL_H
#define NP_VMPIPE_SHM_TEXTURE_INTERNAL_H

#include "vk_surface_buffer.h"

#include <stdint.h>
#include <wayland-server-core.h>

struct np_server;

struct np_shm_texture {
	struct wl_list link;
	struct wl_resource *buffer;
	struct wl_listener buffer_destroy;
	struct np_server *server;
	struct np_vk_surface_buffer image;
	uint32_t width, height, stride, format;
	uint32_t references;
	uint32_t host_reads;
	uint32_t owner_surface_id;
	uint64_t owner_epoch;
	uint64_t content_serial;
};

#endif
