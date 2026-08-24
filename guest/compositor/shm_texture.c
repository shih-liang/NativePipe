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

/* Remove only the buffer-cache ownership. Surfaces and in-flight host reads
 * retain the copied pixels independently. */
static void evict_texture(struct np_shm_texture *texture)
{
	if (!texture) return;
	texture->buffer = NULL;
	if (!wl_list_empty(&texture->link)) {
		wl_list_remove(&texture->link);
		wl_list_init(&texture->link);
	}
	if (!wl_list_empty(&texture->buffer_destroy.link)) {
		wl_list_remove(&texture->buffer_destroy.link);
		wl_list_init(&texture->buffer_destroy.link);
	}
	np_shm_texture_unref(texture);
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
	uint32_t width, uint32_t height, uint32_t format)
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
	texture->format = format;
	texture->references = 1; /* cache/list ownership */
	/* Source and destination row pitches are independent.  A valid wl_shm
	 * buffer may include more padding than the linear VkImage; only width * 4
	 * bytes are copied from each source row. */
	if (!np_vk_surface_buffer_create(width, height, &texture->image)) {
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

enum np_shm_upload_result np_shm_texture_upload(
	struct np_surface *surface, struct wl_resource *buffer,
	const struct np_box *damage, struct np_shm_texture **texture_out)
{
	if (texture_out) *texture_out = NULL;
	if (!surface || !buffer || !texture_out) return NP_SHM_UPLOAD_INVALID;
	struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
	if (!shm) return NP_SHM_UPLOAD_INVALID;
	struct np_server *server = surface->server;
	int32_t width_value = wl_shm_buffer_get_width(shm);
	int32_t height_value = wl_shm_buffer_get_height(shm);
	int32_t stride_value = wl_shm_buffer_get_stride(shm);
	uint32_t format = wl_shm_buffer_get_format(shm);
	if (width_value <= 0 || height_value <= 0 || stride_value <= 0 ||
	    (uint64_t)stride_value < (uint64_t)width_value * 4u ||
	    (format != WL_SHM_FORMAT_ARGB8888 &&
	     format != WL_SHM_FORMAT_XRGB8888))
		return NP_SHM_UPLOAD_INVALID;
	uint32_t width = (uint32_t)width_value;
	uint32_t height = (uint32_t)height_value;
	uint32_t source_stride = (uint32_t)stride_value;

	struct np_shm_texture *texture = find_texture(server, buffer);
	bool fresh = texture == NULL;
	bool mutable_by_surface = texture && !texture->host_reads &&
		(texture->references == 1 ||
		 (texture->references == 2 && surface->current_shm == texture));
	if (texture && !mutable_by_surface) {
		/* wl_buffer.release was sent as soon as the guest copy completed, so the
		 * client is allowed to rewrite and recommit the same wl_shm buffer while
		 * Metal still reads the previous copy. The same wl_buffer may also be
		 * current on more than one surface. Preserve either shared snapshot and
		 * install a fresh cache entry instead of mutating or stalling it. */
		evict_texture(texture);
		texture = NULL;
		fresh = true;
	}
	if (!texture)
		texture = create_texture(
				server, buffer, width, height, format);
	if (!texture) return NP_SHM_UPLOAD_NO_MEMORY;
	if (texture->width != width || texture->height != height ||
	    texture->format != format)
		return NP_SHM_UPLOAD_INVALID;

	int64_t x = damage ? damage->x : 0;
	int64_t y = damage ? damage->y : 0;
	int64_t w = damage ? damage->width : 0;
	int64_t h = damage ? damage->height : 0;
	if (fresh) {
		x = 0; y = 0; w = width; h = height;
	} else if (w <= 0 || h <= 0) {
		/* Reattaching a cached wl_buffer without damage does not change its
		 * contents.  Reuse the copied texture instead of copying every row. */
		*texture_out = texture;
		return NP_SHM_UPLOAD_OK;
	}
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x >= width || y >= height) {
		*texture_out = texture;
		return NP_SHM_UPLOAD_OK;
	}
	if (w > (int64_t)width - x) w = (int64_t)width - x;
	if (h > (int64_t)height - y) h = (int64_t)height - y;
	if (w <= 0 || h <= 0) {
		*texture_out = texture;
		return NP_SHM_UPLOAD_OK;
	}

	wl_shm_buffer_begin_access(shm);
	const unsigned char *source = wl_shm_buffer_get_data(shm);
	if (!source) {
		wl_shm_buffer_end_access(shm);
		return NP_SHM_UPLOAD_INVALID;
	}
	unsigned char *destination = texture->image.mapped;
	const size_t offset = (size_t)x * 4u;
	const size_t span = (size_t)w * 4u;
	for (int64_t row = y; row < y + h; row++) {
		memcpy(destination + (size_t)row * texture->stride + offset,
		       source + (size_t)row * source_stride + offset, span);
	}
	wl_shm_buffer_end_access(shm);
	np_vk_surface_buffer_flush(&texture->image);
	*texture_out = texture;
	return NP_SHM_UPLOAD_OK;
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
