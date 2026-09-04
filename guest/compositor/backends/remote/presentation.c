/* Remote backend publication: copy committed pixels into immutable encoded
 * resources.  Wayland scene and callback scheduling remain in the frontend. */

#define _GNU_SOURCE

#include "backend.h"
#include "backend_internal.h"
#include "compositor_internal.h"
#include "dmabuf.h"
#include "media.h"
#include "scene.h"
#include "surface_internal.h"
#include "syncobj.h"
#include "window_events.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server-protocol.h>

static uint64_t monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static struct np_remote_surface *remote_surface(
	struct np_surface *surface, bool create)
{
	if (!surface) return NULL;
	struct np_remote_surface *state = surface->backend_surface_state;
	if (!state && create) {
		state = calloc(1, sizeof(*state));
		surface->backend_surface_state = state;
	}
	return state;
}

static void encoder_emit(
	void *user, const uint8_t *data, size_t size, uint64_t pts_ns,
	uint32_t resource_id, uint8_t flags, const uint8_t *alpha,
	uint32_t alpha_size, uint16_t bitstream_epoch,
	uint16_t width, uint16_t height)
{
	struct np_surface *surface = user;
	if (!surface || !surface->server) return;
	struct np_remote_backend *backend = np_remote_backend(surface->server);
	if (!backend) return;
	if (alpha && alpha_size)
		(void)np_media_send(
			&backend->media, NP_MEDIA_CODEC_ALPHA_RLE, 0,
			surface->id, resource_id, width, height, pts_ns,
			bitstream_epoch, alpha, alpha_size);
	(void)np_media_send(
		&backend->media, NP_MEDIA_CODEC_H264, flags,
		surface->id, resource_id, width, height, pts_ns,
		bitstream_epoch, data, (uint32_t)size);
}

static uint32_t next_media_resource_id(struct np_server *server)
{
	struct np_remote_backend *backend = np_remote_backend(server);
	if (!backend) return 0;
	uint32_t id = ++backend->next_media_resource_id;
	if (!id) id = ++backend->next_media_resource_id;
	return id;
}

static bool source_has_transparency(
	const unsigned char *source, int32_t width, int32_t height, int32_t stride)
{
	for (int32_t row = 0; row < height; row++) {
		const unsigned char *pixel = source + (size_t)row * (size_t)stride + 3;
		for (int32_t column = 0; column < width; column++, pixel += 4) {
			if (*pixel != 255) return true;
		}
	}
	return false;
}

static bool encode_remote_pixels(
	struct np_surface *surface, const unsigned char *source,
	int32_t width, int32_t height, int32_t stride, uint32_t format)
{
	if (!source || width < 2 || height < 2 || width >= UINT16_MAX ||
	    height >= UINT16_MAX || width > INT32_MAX / 4 || stride < width * 4)
		return false;
	if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888)
		return false;
	struct np_remote_surface *state = remote_surface(surface, true);
	if (!state) return false;
	if (!state->encoder) {
		state->encoder = np_encoder_create(
			(width + 1) & ~1, (height + 1) & ~1, encoder_emit, surface);
		if (!state->encoder) return false;
	}

	uint32_t resource_id = next_media_resource_id(surface->server);
	uint8_t flags = format == WL_SHM_FORMAT_ARGB8888 &&
	                source_has_transparency(source, width, height, stride)
		? NP_MEDIA_FLAG_HAS_ALPHA : 0;
	if (!np_encoder_push_bgra(
			state->encoder, source, width, height, stride, monotonic_ns(),
			resource_id, flags))
		return false;
	surface->last_width = width;
	surface->last_height = height;
	surface->last_stride = (uint32_t)(width * 4);
	surface->last_format = format == WL_SHM_FORMAT_ARGB8888
		? "bgra8888" : "bgrx8888";
	surface->last_source = "encoded";
	surface->last_resource_id = resource_id;
	state->last_epoch = np_encoder_epoch(state->encoder);
	surface->has_published = true;
	return true;
}

static bool publish_frame_remote(
	struct np_surface *surface, struct wl_shm_buffer *shm)
{
	wl_shm_buffer_begin_access(shm);
	bool encoded = encode_remote_pixels(
		surface, wl_shm_buffer_get_data(shm), wl_shm_buffer_get_width(shm),
		wl_shm_buffer_get_height(shm), wl_shm_buffer_get_stride(shm),
		wl_shm_buffer_get_format(shm));
	wl_shm_buffer_end_access(shm);
	return encoded;
}

bool np_backend_refresh_current_shm(
	struct np_surface *surface, uint32_t presentation_id,
	const struct np_box *damage)
{
	(void)damage;
	struct wl_shm_buffer *shm = wl_shm_buffer_get(surface->current_buffer);
	if (!shm || !publish_frame_remote(surface, shm)) return false;
	if (np_scene_root(surface))
		np_presentation_queue_scene(surface, presentation_id);
	else if (!np_presentation_queue_last(surface, presentation_id))
		np_presentation_request_refresh(surface, presentation_id);
	return true;
}

bool np_remote_republish_surface(struct np_surface *surface)
{
	struct np_remote_surface *state = remote_surface(surface, false);
	if (!surface || !state || !state->encoder || !surface->has_published ||
	    !surface->last_resource_id)
		return false;
	uint32_t replacement = next_media_resource_id(surface->server);
	uint32_t active = 0;
	if (np_encoder_replay_last(
			state->encoder, monotonic_ns(), replacement, &active) && active)
		surface->last_resource_id = active;
	else
		np_encoder_force_keyframe(state->encoder);
	state->last_epoch = np_encoder_epoch(state->encoder);
	uint32_t presentation_id = np_presentation_next_id(surface->server);
	if (np_scene_root(surface))
		np_presentation_queue_scene(surface, presentation_id);
	else
		(void)np_presentation_queue_last(surface, presentation_id);
	return true;
}

void np_remote_force_keyframe(struct np_surface *surface)
{
	struct np_remote_surface *state = remote_surface(surface, false);
	if (state && state->encoder) np_encoder_force_keyframe(state->encoder);
}

void np_backend_surface_destroy(struct np_surface *surface)
{
	struct np_remote_surface *state = remote_surface(surface, false);
	if (!state) return;
	if (state->encoder) np_encoder_destroy(state->encoder);
	free(state);
	surface->backend_surface_state = NULL;
}

void np_backend_publish_buffer(
	struct np_surface *surface, struct wl_resource *buffer,
	struct np_gpu_buffer *gpu_buffer,
	enum np_buffer_commit_kind buffer_commit, uint32_t presentation_id,
	struct np_sync_point *release_point, const struct np_box *damage)
{
	struct np_surface *root = np_scene_root(surface);
	(void)damage;
	if (buffer_commit == NP_BUFFER_DETACH) {
		if (surface == root && surface->mapped) {
			uint32_t fields[] = {surface->id};
			np_window_event_send(surface->server, NP_GUEST_SURFACE_UNMAPPED,
			                     fields, 1);
			surface->mapped = false;
		}
		np_presentation_set_current_buffer(surface, NULL, NULL, release_point);
		surface->has_published = false;
		if (root && surface != root)
			np_presentation_queue_scene(surface, presentation_id);
		else
			np_presentation_request_refresh(surface, presentation_id);
		return;
	}

	struct wl_shm_buffer *shm = buffer ? wl_shm_buffer_get(buffer) : NULL;
	struct np_gpu_buffer *gpu = gpu_buffer ? gpu_buffer : np_gpu_buffer_get(buffer);
	bool encoded = false;
	if (shm) {
		encoded = publish_frame_remote(surface, shm);
	} else if (gpu) {
		const unsigned char *pixels = NULL;
		if (np_gpu_buffer_begin_cpu_read(gpu, &pixels)) {
			encoded = encode_remote_pixels(
				surface, pixels, gpu->width, gpu->height,
				gpu->stride, gpu->format);
			np_gpu_buffer_end_cpu_read(gpu);
		}
	}
	if (!encoded) {
		np_sync_point_signal(release_point);
		if (buffer) wl_buffer_send_release(buffer);
		wl_client_post_implementation_error(
			wl_resource_get_client(surface->resource),
			"could not copy the committed buffer into the remote encoder");
		return;
	}

	np_presentation_set_current_buffer(surface, buffer, gpu, release_point);
	if (buffer) wl_buffer_send_release(buffer);
	if (root) {
		if (surface == root) surface->mapped = true;
		np_presentation_queue_scene(surface, presentation_id);
	} else if (!np_presentation_queue_last(surface, presentation_id)) {
		np_presentation_request_refresh(surface, presentation_id);
	}
}
