#include "shm_texture.h"

#include "compositor_internal.h"
#include "shm_texture_internal.h"
#include "vk_surface_buffer.h"

#include <stdio.h>
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

static void reset_damage_history(
	struct np_surface *surface, uint32_t width, uint32_t height)
{
	if (++surface->shm_damage_epoch == 0) surface->shm_damage_epoch = 1;
	surface->shm_damage_serial = 0;
	surface->shm_damage_width = width;
	surface->shm_damage_height = height;
	surface->shm_damage_geometry_valid = width > 0 && height > 0;
	surface->shm_damage_history_count = 0;
}

void np_shm_texture_prepare_commit(
	struct np_surface *surface, uint32_t width, uint32_t height,
	bool mapping_changed)
{
	if (!surface) return;
	bool valid = width > 0 && height > 0;
	if (mapping_changed || surface->shm_damage_geometry_valid != valid ||
	    (valid && (surface->shm_damage_width != width ||
	               surface->shm_damage_height != height)))
		reset_damage_history(surface, width, height);
}

void np_shm_texture_note_damage(
	struct np_surface *surface, const struct np_box *damage)
{
	if (!surface || !damage || damage->width <= 0 || damage->height <= 0)
		return;
	if (surface->shm_damage_serial == UINT64_MAX)
		reset_damage_history(
			surface, surface->shm_damage_width, surface->shm_damage_height);
	uint32_t count = surface->shm_damage_history_count;
	if (count == NP_SHM_DAMAGE_HISTORY_CAPACITY) {
		memmove(&surface->shm_damage_history[0],
		        &surface->shm_damage_history[1],
		        sizeof(surface->shm_damage_history[0]) * (count - 1));
		count--;
	}
	struct np_shm_damage_record *record =
		&surface->shm_damage_history[count++];
	record->serial = ++surface->shm_damage_serial;
	record->damage = *damage;
	surface->shm_damage_history_count = count;
}

static bool collect_cached_damage(
	const struct np_surface *surface, const struct np_shm_texture *texture,
	const struct np_box *current, struct np_box *result)
{
	np_box_clear(result);
	if (!surface->shm_damage_geometry_valid ||
	    texture->owner_surface_id != surface->id ||
	    texture->owner_epoch != surface->shm_damage_epoch ||
	    texture->content_serial > surface->shm_damage_serial)
		return false;

	if (texture->content_serial < surface->shm_damage_serial) {
		uint32_t count = surface->shm_damage_history_count;
		if (!count || surface->shm_damage_history[0].serial >
		              texture->content_serial + 1)
			return false;
		for (uint32_t i = 0; i < count; i++) {
			const struct np_shm_damage_record *record =
				&surface->shm_damage_history[i];
			if (record->serial <= texture->content_serial) continue;
			np_box_union(result, record->damage.x, record->damage.y,
			             record->damage.width, record->damage.height);
		}
	}
	if (current)
		np_box_union(result, current->x, current->y,
		             current->width, current->height);
	return true;
}

static uint64_t content_serial_after_commit(
	const struct np_surface *surface, const struct np_box *damage)
{
	return surface->shm_damage_serial +
		(damage && damage->width > 0 && damage->height > 0 ? 1u : 0u);
}

static void mark_texture_current(
	struct np_shm_texture *texture, const struct np_surface *surface,
	const struct np_box *damage)
{
	texture->owner_surface_id = surface->id;
	texture->owner_epoch = surface->shm_damage_epoch;
	texture->content_serial = content_serial_after_commit(surface, damage);
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
	if (damage && damage->width > 0 && damage->height > 0 &&
	    surface->shm_damage_serial == UINT64_MAX)
		reset_damage_history(surface, width, height);

	struct np_shm_texture *texture = find_texture(server, buffer);
	bool fresh = texture == NULL;
	bool copied_on_write = false;
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
		copied_on_write = true;
	}
	if (!texture)
		texture = create_texture(
				server, buffer, width, height, format);
	if (!texture) return NP_SHM_UPLOAD_NO_MEMORY;
	if (texture->width != width || texture->height != height ||
	    texture->format != format)
		return NP_SHM_UPLOAD_INVALID;

	struct np_box upload;
	bool cache_history_valid = !fresh &&
		collect_cached_damage(surface, texture, damage, &upload);
	if (!cache_history_valid) {
		upload = (struct np_box){0, 0, width, height};
	}
	int64_t x = upload.x;
	int64_t y = upload.y;
	int64_t w = upload.width;
	int64_t h = upload.height;
	if (!cache_history_valid) {
		x = 0; y = 0; w = width; h = height;
	} else if (w <= 0 || h <= 0) {
		/* No surface transition occurred since this cache entry was current. */
		mark_texture_current(texture, surface, damage);
		*texture_out = texture;
		if (np_trace_enabled())
			fprintf(stderr,
			        "[shm] surface=%u buffer=%u resource=%u reuse no-damage\n",
			        surface->id, wl_resource_get_id(buffer),
			        texture->image.resource_id);
		return NP_SHM_UPLOAD_OK;
	}
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x >= width || y >= height) {
		mark_texture_current(texture, surface, damage);
		*texture_out = texture;
		return NP_SHM_UPLOAD_OK;
	}
	if (w > (int64_t)width - x) w = (int64_t)width - x;
	if (h > (int64_t)height - y) h = (int64_t)height - y;
	if (w <= 0 || h <= 0) {
		mark_texture_current(texture, surface, damage);
		*texture_out = texture;
		return NP_SHM_UPLOAD_OK;
	}
	if (np_trace_enabled()) {
		uint64_t target_serial = content_serial_after_commit(surface, damage);
		uint64_t age = target_serial >= texture->content_serial
			? target_serial - texture->content_serial : 0;
		fprintf(stderr,
		        "[shm] surface=%u buffer=%u resource=%u %s age=%llu "
		        "copy=%lld,%lld %lldx%lld\n",
		        surface->id, wl_resource_get_id(buffer), texture->image.resource_id,
		        copied_on_write ? "cow" : fresh ? "fresh" :
		        cache_history_valid ? "aged" : "reset",
		        (unsigned long long)age,
		        (long long)x, (long long)y, (long long)w, (long long)h);
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
	mark_texture_current(texture, surface, damage);
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
