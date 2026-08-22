#ifndef NP_SHM_TEXTURE_H
#define NP_SHM_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

#ifndef NP_REMOTE
#include "vk_surface_buffer.h"
#endif

struct np_box;
struct np_server;
struct wl_resource;

#ifndef NP_REMOTE
/// Compositor-owned texture associated with one wl_shm wl_buffer. Client bytes
/// are copied once at commit; the host only ever sees this Vulkan resource.
struct np_shm_texture {
	struct wl_list link;
	struct wl_resource *buffer;
	struct wl_listener buffer_destroy;
	struct np_server *server;
	struct np_vk_surface_buffer image;
	uint32_t width, height, stride, format;
	uint32_t references;
	uint32_t host_reads;
};

struct np_shm_texture *np_shm_texture_upload(
	struct np_server *server, struct wl_resource *buffer,
	const struct np_box *damage);
bool np_shm_texture_is_busy(struct np_server *server,
	                         struct wl_resource *buffer);
void np_shm_texture_ref(struct np_shm_texture *texture);
void np_shm_texture_unref(struct np_shm_texture *texture);
void np_shm_texture_begin_host_read(struct np_shm_texture *texture);
void np_shm_texture_end_host_read(struct np_shm_texture *texture);
void np_shm_texture_finish_all(struct np_server *server);
#else
struct np_shm_texture;
static inline void np_shm_texture_ref(struct np_shm_texture *texture)
{ (void)texture; }
static inline void np_shm_texture_unref(struct np_shm_texture *texture)
{ (void)texture; }
#endif

#endif
