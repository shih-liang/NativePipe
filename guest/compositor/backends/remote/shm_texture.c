#include "shm_texture.h"

enum np_shm_upload_result np_shm_texture_upload(
	struct np_surface *surface, struct wl_resource *buffer,
	const struct np_box *damage, struct np_shm_texture **texture_out)
{
	(void)surface;
	(void)buffer;
	(void)damage;
	if (texture_out) *texture_out = 0;
	return NP_SHM_UPLOAD_INVALID;
}

void np_shm_texture_prepare_commit(
	struct np_surface *surface, uint32_t width, uint32_t height,
	bool mapping_changed)
{
	(void)surface;
	(void)width;
	(void)height;
	(void)mapping_changed;
}

void np_shm_texture_note_damage(
	struct np_surface *surface, const struct np_box *damage)
{
	(void)surface;
	(void)damage;
}

void np_shm_texture_ref(struct np_shm_texture *texture) { (void)texture; }
void np_shm_texture_unref(struct np_shm_texture *texture) { (void)texture; }
void np_shm_texture_begin_host_read(struct np_shm_texture *texture)
{
	(void)texture;
}
void np_shm_texture_end_host_read(struct np_shm_texture *texture)
{
	(void)texture;
}
void np_shm_texture_finish_all(struct np_server *server) { (void)server; }
