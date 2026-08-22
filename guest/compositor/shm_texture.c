#include "shm_texture.h"

#include "compositor_internal.h"
#include "vk_surface_buffer.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-protocol.h>

static struct np_shm_texture *find_texture(
	struct np_server *server, struct wl_resource *buffer)
{
	struct np_shm_texture *texture;
	wl_list_for_each(texture, &server->shm_textures, link) {
		if (texture->buffer == buffer) return texture;
	}
	return NULL;
}

void np_shm_texture_ref(struct np_shm_texture *texture)
{
	if (texture) texture->references++;
}

void np_shm_texture_unref(struct np_shm_texture *texture)
{
	if (!texture || !texture->references || --texture->references) return;
	if (!wl_list_empty(&texture->link)) wl_list_remove(&texture->link);
	if (!wl_list_empty(&texture->buffer_destroy.link))
		wl_list_remove(&texture->buffer_destroy.link);
	np_vk_surface_buffer_destroy(&texture->image);
	free(texture);
}

static void buffer_destroyed(struct wl_listener *listener, void *data)
{
	(void)data;
	struct np_shm_texture *texture =
		wl_container_of(listener, texture, buffer_destroy);
	texture->buffer = NULL;
	wl_list_remove(&texture->buffer_destroy.link);
	wl_list_init(&texture->buffer_destroy.link);
	if (!wl_list_empty(&texture->link)) {
		wl_list_remove(&texture->link);
		wl_list_init(&texture->link);
	}
	/* Drop the cache/list reference. Current surfaces and presentations retain
	 * their own references and therefore keep the copied pixels alive. */
	np_shm_texture_unref(texture);
}

static struct np_shm_texture *create_texture(
	struct np_server *server, struct wl_resource *buffer,
	uint32_t width, uint32_t height, uint32_t stride, uint32_t format)
{
	struct np_shm_texture *texture = calloc(1, sizeof(*texture));
	if (!texture) return NULL;
	wl_list_init(&texture->link);
	wl_list_init(&texture->buffer_destroy.link);
	texture->image.resource_drm_fd = -1;
	texture->server = server;
	texture->buffer = buffer;
	texture->width = width;
	texture->height = height;
	texture->stride = stride;
	texture->format = format;
	texture->references = 1; /* cache/list ownership */
	if (!np_vk_surface_buffer_create(width, height, &texture->image) ||
	    texture->image.stride < stride) {
		np_vk_surface_buffer_destroy(&texture->image);
		free(texture);
		return NULL;
	}
	texture->stride = texture->image.stride;
	texture->buffer_destroy.notify = buffer_destroyed;
	wl_resource_add_destroy_listener(buffer, &texture->buffer_destroy);
	wl_list_insert(&server->shm_textures, &texture->link);
	return texture;
}

bool np_shm_texture_is_busy(struct np_server *server,
	                         struct wl_resource *buffer)
{
	struct np_shm_texture *texture = find_texture(server, buffer);
	return texture && texture->host_reads != 0;
}

struct np_shm_texture *np_shm_texture_upload(
	struct np_server *server, struct wl_resource *buffer,
	const struct np_box *damage)
{
	struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
	if (!server || !buffer || !shm) return NULL;
	uint32_t width = (uint32_t)wl_shm_buffer_get_width(shm);
	uint32_t height = (uint32_t)wl_shm_buffer_get_height(shm);
	uint32_t source_stride = (uint32_t)wl_shm_buffer_get_stride(shm);
	uint32_t format = wl_shm_buffer_get_format(shm);
	if (!width || !height || source_stride < width * 4u ||
	    (format != WL_SHM_FORMAT_ARGB8888 &&
	     format != WL_SHM_FORMAT_XRGB8888))
		return NULL;

	struct np_shm_texture *texture = find_texture(server, buffer);
	bool fresh = texture == NULL;
	if (!texture)
		texture = create_texture(
			server, buffer, width, height, source_stride, format);
	if (!texture || texture->host_reads || texture->width != width ||
	    texture->height != height || texture->format != format)
		return NULL;

	int32_t x = damage ? damage->x : 0;
	int32_t y = damage ? damage->y : 0;
	int32_t w = damage ? damage->width : 0;
	int32_t h = damage ? damage->height : 0;
	if (fresh || w <= 0 || h <= 0) {
		x = 0; y = 0; w = (int32_t)width; h = (int32_t)height;
	}
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > (int32_t)width) w = (int32_t)width - x;
	if (y + h > (int32_t)height) h = (int32_t)height - y;
	if (w <= 0 || h <= 0) return texture;

	wl_shm_buffer_begin_access(shm);
	const unsigned char *source = wl_shm_buffer_get_data(shm);
	if (!source) {
		wl_shm_buffer_end_access(shm);
		return NULL;
	}
	unsigned char *destination = texture->image.mapped;
	const size_t offset = (size_t)x * 4u;
	const size_t span = (size_t)w * 4u;
	for (int32_t row = y; row < y + h; row++) {
		memcpy(destination + (size_t)row * texture->stride + offset,
		       source + (size_t)row * source_stride + offset, span);
	}
	wl_shm_buffer_end_access(shm);
	np_vk_surface_buffer_flush(&texture->image);
	return texture;
}

void np_shm_texture_begin_host_read(struct np_shm_texture *texture)
{
	if (!texture) return;
	texture->host_reads++;
	np_shm_texture_ref(texture);
}

void np_shm_texture_end_host_read(struct np_shm_texture *texture)
{
	if (!texture || !texture->host_reads) return;
	texture->host_reads--;
	np_shm_texture_unref(texture);
}

void np_shm_texture_finish_all(struct np_server *server)
{
	struct np_shm_texture *texture, *tmp;
	wl_list_for_each_safe(texture, tmp, &server->shm_textures, link) {
		texture->buffer = NULL;
		wl_list_remove(&texture->link);
		wl_list_init(&texture->link);
		if (!wl_list_empty(&texture->buffer_destroy.link)) {
			wl_list_remove(&texture->buffer_destroy.link);
			wl_list_init(&texture->buffer_destroy.link);
		}
		np_shm_texture_unref(texture);
	}
}
