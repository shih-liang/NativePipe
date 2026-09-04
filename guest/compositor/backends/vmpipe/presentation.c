/* VMPipe buffer publication: preserve original virtio-gpu and wl_shm
 * lifetimes while the frontend owns Wayland commit/scene scheduling. */

#include "compositor_internal.h"
#include "dmabuf.h"
#include "scale.h"
#include "scene.h"
#include "shm_texture.h"
#include "shm_texture_internal.h"
#include "syncobj.h"
#include "window_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

static void set_current_shm(
	struct np_surface *surface, struct np_shm_texture *texture)
{
	if (surface->current_shm == texture) return;
	if (surface->current_shm) np_shm_texture_unref(surface->current_shm);
	surface->current_shm = texture;
	if (texture) np_shm_texture_ref(texture);
}

/* Cursor and drag-icon surfaces have no xdg root, so they use the low-rate
 * committed control message. Their pixels still follow the same direct source
 * lifetime as a scene layer; this is not a second window rendering path. */
static bool queue_unroled_frame(
	struct np_surface *surface, struct wl_resource *buffer,
	uint32_t presentation_id, struct np_sync_point *release_point,
	const struct np_box *damage)
{
	struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
	struct np_gpu_buffer *gpu = np_gpu_buffer_get(buffer);
	uint32_t resource_id = 0, stride = 0, format = 0;
	int32_t width = 0, height = 0;
	const char *source = NULL;
	if (shm) {
		struct np_shm_texture *texture = NULL;
		enum np_shm_upload_result result = np_shm_texture_upload(
			surface, buffer, damage, &texture);
		if (result != NP_SHM_UPLOAD_OK) {
			np_sync_point_signal(release_point);
			wl_buffer_send_release(buffer);
			if (result == NP_SHM_UPLOAD_NO_MEMORY)
				wl_client_post_no_memory(wl_resource_get_client(surface->resource));
			else
				wl_client_post_implementation_error(
					wl_resource_get_client(surface->resource),
					"could not import committed cursor or drag wl_shm buffer");
			return false;
		}
		np_presentation_set_current_buffer(surface, buffer, NULL, release_point);
		set_current_shm(surface, texture);
		resource_id = texture->image.resource_id;
		stride = texture->stride;
		width = wl_shm_buffer_get_width(shm);
		height = wl_shm_buffer_get_height(shm);
		format = wl_shm_buffer_get_format(shm);
		source = "cpu";
		/* The guest copy is complete; subsequent host access targets texture. */
		wl_buffer_send_release(buffer);
	} else if (gpu) {
		np_presentation_set_current_buffer(surface, buffer, gpu, release_point);
		resource_id = gpu->resource_id;
		stride = (uint32_t)gpu->stride;
		width = gpu->width;
		height = gpu->height;
		format = gpu->format;
		source = "gpu";
	} else {
		np_sync_point_signal(release_point);
		wl_client_post_implementation_error(
			wl_resource_get_client(surface->resource),
			"cursor or drag surface buffer is neither wl_shm nor linux-dmabuf");
		return false;
	}
	int wait_fd = -1;
	enum np_scene_build_result hold =
		np_scene_hold_current(surface, presentation_id, &wait_fd);
	if (hold != NP_SCENE_READY) {
		if (wait_fd >= 0) close(wait_fd);
		if (hold == NP_SCENE_NO_MEMORY)
			wl_client_post_no_memory(wl_resource_get_client(surface->resource));
		else
			wl_client_post_implementation_error(
				wl_resource_get_client(surface->resource),
				"could not retain committed cursor or drag buffer");
		return false;
	}

	uint8_t format_value = format == WL_SHM_FORMAT_XRGB8888 ||
	                       format == 0x34325258u ? NP_WINDOW_BGRX8888 :
	                       format == 0x34324241u ||
	                       format == 0x34324258u ? NP_WINDOW_RGBA8888 :
	                       NP_WINDOW_BGRA8888;
	struct np_window_frame frame = {
		.resource_id = resource_id,
		.width = width,
		.height = height,
		.bytes_per_row = (int32_t)stride,
		.scale = surface->scale,
		.format = format_value,
		.source = shm ? NP_WINDOW_FRAME_CPU : NP_WINDOW_FRAME_GPU,
		.presentation_id = presentation_id,
	};
	np_presentation_add_viewport(surface, &frame);
	unsigned char *body = NULL;
	size_t body_size = 0;
	if (!np_window_event_send_frame(
		    surface->server, surface->id, &frame, &body, &body_size)) {
		np_scene_presented(surface, presentation_id);
		return false;
	}
	if (surface->pending_frame) {
		uint32_t old = surface->pending_presentation_id;
		np_presentation_rebind_callbacks(surface, old, presentation_id);
		np_scene_presented(surface, old);
		free(surface->pending_frame);
	}
	surface->last_resource_id = resource_id;
	surface->last_width = width;
	surface->last_height = height;
	surface->last_stride = stride;
	surface->last_format = format_value == NP_WINDOW_BGRX8888 ? "bgrx8888" :
	                       format_value == NP_WINDOW_RGBA8888 ? "rgba8888" :
	                       "bgra8888";
	surface->last_source = source;
	surface->has_published = true;
	surface->pending_frame = body;
	surface->pending_frame_size = body_size;
	surface->pending_presentation_id = presentation_id;
	return true;
}

bool np_backend_refresh_current_shm(
	struct np_surface *surface, uint32_t presentation_id,
	const struct np_box *damage)
{
	struct np_shm_texture *texture = NULL;
	enum np_shm_upload_result result = np_shm_texture_upload(
		surface, surface->current_buffer, damage, &texture);
	if (result == NP_SHM_UPLOAD_OK) {
		set_current_shm(surface, texture);
		if (!np_presentation_queue_last(surface, presentation_id))
			np_presentation_request_refresh(surface, presentation_id);
		return true;
	}
	if (result == NP_SHM_UPLOAD_NO_MEMORY)
		wl_client_post_no_memory(wl_resource_get_client(surface->resource));
	else
		wl_client_post_implementation_error(
			wl_resource_get_client(surface->resource),
			"could not update current wl_shm buffer");
	return false;
}

void np_backend_publish_buffer(
	struct np_surface *surface, struct wl_resource *buffer,
	struct np_gpu_buffer *gpu_buffer,
	enum np_buffer_commit_kind buffer_commit, uint32_t presentation_id,
	struct np_sync_point *release_point, const struct np_box *damage)
{
	struct np_surface *root = np_scene_root(surface);
	if (np_trace_enabled())
		fprintf(stderr,
		        "[wayland] publish surface=%u root=%u top=%d popup=%d sub=%d buffer=%s\n",
		        surface->id, root ? root->id : 0, surface->toplevel != NULL,
		        surface->popup != NULL, surface->subsurface != NULL,
		        buffer_commit == NP_BUFFER_DETACH ? "gone" :
		        buffer ? "set" : "retained");
	if (root) {
		if (buffer_commit == NP_BUFFER_DETACH) {
			if (surface == root && surface->mapped) {
				uint32_t fields[] = {surface->id};
				np_window_event_send(surface->server,
				                     NP_GUEST_SURFACE_UNMAPPED, fields, 1);
				surface->mapped = false;
			}
			np_presentation_set_current_buffer(surface, NULL, NULL, release_point);
			surface->has_published = false;
			if (surface == root)
				np_presentation_request_refresh(surface, presentation_id);
			else
				np_presentation_queue_scene(surface, presentation_id);
			return;
		}

		struct wl_shm_buffer *shm = buffer ? wl_shm_buffer_get(buffer) : NULL;
		struct np_gpu_buffer *gpu = gpu_buffer ? gpu_buffer : np_gpu_buffer_get(buffer);
		if (shm) {
			struct np_shm_texture *texture = NULL;
			enum np_shm_upload_result result = np_shm_texture_upload(
				surface, buffer, damage, &texture);
			if (result != NP_SHM_UPLOAD_OK) {
				np_sync_point_signal(release_point);
				wl_buffer_send_release(buffer);
				if (result == NP_SHM_UPLOAD_NO_MEMORY)
					wl_client_post_no_memory(wl_resource_get_client(surface->resource));
				else
					wl_client_post_implementation_error(
						wl_resource_get_client(surface->resource),
						"could not import committed wl_shm buffer");
				return;
			}
			np_presentation_set_current_buffer(surface, buffer, NULL, release_point);
			set_current_shm(surface, texture);
			surface->last_width = wl_shm_buffer_get_width(shm);
			surface->last_height = wl_shm_buffer_get_height(shm);
			surface->last_stride = texture->stride;
			uint32_t format = wl_shm_buffer_get_format(shm);
			surface->last_format = format == WL_SHM_FORMAT_ARGB8888 ? "bgra8888"
				: format == WL_SHM_FORMAT_XRGB8888 ? "bgrx8888"
				: "rgba8888";
			surface->last_source = "cpu";
			surface->last_resource_id = texture->image.resource_id;
			wl_buffer_send_release(buffer);
		} else if (gpu) {
			np_presentation_set_current_buffer(surface, buffer, gpu, release_point);
			surface->last_width = gpu->width;
			surface->last_height = gpu->height;
			surface->last_stride = (uint32_t)gpu->stride;
			surface->last_format = gpu->format == 0x34325241 ? "bgra8888"
				: gpu->format == 0x34325258 ? "bgrx8888"
				: gpu->format == 0x34324241 ? "rgba8888"
				: "rgba8888";
			surface->last_source = "gpu";
			surface->last_resource_id = gpu->resource_id;
		} else {
			np_sync_point_signal(release_point);
			if (np_trace_enabled())
				fprintf(stderr, "[wayland] scene surface=%u: unsupported buffer\n",
				        surface->id);
			return;
		}
		if (np_trace_enabled()) {
			struct np_surface_mapping mapping;
			if (np_scale_resolve(surface, (uint32_t)surface->last_width,
			                     (uint32_t)surface->last_height, &mapping))
				fprintf(stderr,
				        "[wayland] published surface=%u resource=%u pixels=%dx%d "
				        "scale=%d logical=%.0fx%.0f\n",
				        surface->id, surface->last_resource_id,
				        surface->last_width, surface->last_height, surface->scale,
				        mapping.logical_width, mapping.logical_height);
		}
		surface->has_published = true;
		if (surface == root) surface->mapped = true;
		np_presentation_queue_scene(surface, presentation_id);
		return;
	}

	/* A role object may be destroyed before the wl_surface is unmapped. GTK
	 * popovers do this, then reuse the same wl_surface for the next popup. */
	if (buffer_commit == NP_BUFFER_DETACH) {
		np_presentation_set_current_buffer(surface, NULL, NULL, release_point);
		surface->has_published = false;
		np_presentation_request_refresh(surface, presentation_id);
		return;
	}
	if (!buffer) {
		np_sync_point_signal(release_point);
		np_presentation_request_refresh(surface, presentation_id);
		return;
	}

	if (!queue_unroled_frame(
			surface, buffer, presentation_id, release_point, damage))
		return;
	if (presentation_id && surface->pending_presentation_id != presentation_id)
		np_presentation_request_refresh(surface, presentation_id);
}
