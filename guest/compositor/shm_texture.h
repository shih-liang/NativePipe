#ifndef NP_SHM_TEXTURE_H
#define NP_SHM_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

struct np_box;
struct np_server;
struct np_surface;
struct np_shm_texture;
struct wl_resource;

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
