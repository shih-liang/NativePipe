#ifndef NP_SHM_TEXTURE_H
#define NP_SHM_TEXTURE_H

#ifdef NP_REMOTE

#include <stdbool.h>
#include <stdint.h>

struct np_box;
struct np_server;
struct np_surface;
struct np_shm_texture;
struct wl_resource;

static inline void np_shm_texture_prepare_commit(
	struct np_surface *surface, uint32_t width, uint32_t height,
	bool mapping_changed)
{
	(void)surface; (void)width; (void)height; (void)mapping_changed;
}
static inline void np_shm_texture_note_damage(
	struct np_surface *surface, const struct np_box *damage)
{
	(void)surface; (void)damage;
}
static inline void np_shm_texture_ref(struct np_shm_texture *texture)
{
	(void)texture;
}
static inline void np_shm_texture_unref(struct np_shm_texture *texture)
{
	(void)texture;
}
static inline void np_shm_texture_finish_all(struct np_server *server)
{
	(void)server;
}

#else

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

#include "vk_surface_buffer.h"

struct np_box;
struct np_server;
struct np_surface;
struct wl_resource;

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

enum np_shm_upload_result {
	NP_SHM_UPLOAD_OK = 0,
	NP_SHM_UPLOAD_INVALID,
	NP_SHM_UPLOAD_NO_MEMORY,
};

enum np_shm_upload_result np_shm_texture_upload(
	struct np_surface *surface, struct wl_resource *buffer,
	const struct np_box *damage, struct np_shm_texture **texture_out);
void np_shm_texture_prepare_commit(
	struct np_surface *surface, uint32_t width, uint32_t height,
	bool mapping_changed);
void np_shm_texture_note_damage(
	struct np_surface *surface, const struct np_box *damage);
void np_shm_texture_ref(struct np_shm_texture *texture);
void np_shm_texture_unref(struct np_shm_texture *texture);
void np_shm_texture_begin_host_read(struct np_shm_texture *texture);
void np_shm_texture_end_host_read(struct np_shm_texture *texture);
void np_shm_texture_finish_all(struct np_server *server);

#endif
#endif
