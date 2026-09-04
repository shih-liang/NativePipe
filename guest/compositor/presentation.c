// Frame callbacks, buffer ownership and host-visible presentation.

#define _GNU_SOURCE

#include "backend.h"
#include "compositor_internal.h"
#include "dmabuf.h"
#include "perf.h"
#include "scale.h"
#include "scene.h"
#include "shm_texture.h"
#include "syncobj.h"
#include "window_events.h"
#include "xdg_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

static uint32_t presentation_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void np_presentation_clear_scene_wait(struct np_surface *surface)
{
	if (!surface) return;
	if (surface->scene_wait_source)
		wl_event_source_remove(surface->scene_wait_source);
	if (surface->scene_wait_fd >= 0) close(surface->scene_wait_fd);
	surface->scene_wait_source = NULL;
	surface->scene_wait_fd = -1;
}

static void frame_callback_destroy(struct wl_resource *resource) {
	struct np_frame_callback *callback = wl_resource_get_user_data(resource);
	if (!callback) return;
	if (!wl_list_empty(&callback->link)) wl_list_remove(&callback->link);
	free(callback);
}

static void frame_callback_fire(struct np_frame_callback *callback) {
	if (!callback) return;
	if (!wl_list_empty(&callback->link)) {
		wl_list_remove(&callback->link);
		wl_list_init(&callback->link);
	}
	wl_callback_send_done(callback->resource, presentation_now_ms());
	wl_resource_destroy(callback->resource);
}

void np_surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	struct np_frame_callback *callback = calloc(1, sizeof(*callback));
	if (!callback) {
		wl_client_post_no_memory(client);
		return;
	}
	callback->resource = wl_resource_create(client, &wl_callback_interface, 1, id);
	if (!callback->resource) {
		free(callback);
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&callback->link);
	wl_resource_set_implementation(
		callback->resource, NULL, callback, frame_callback_destroy);
	wl_list_insert(surface->pending_frame_callbacks.prev, &callback->link);
}

uint32_t np_presentation_next_id(struct np_server *server) {
	uint32_t id = ++server->next_presentation_id;
	if (!id) id = ++server->next_presentation_id;
	return id;
}

/// Associate only the callbacks still in pending surface state with this
/// commit. Older callbacks retain the presentation which already owns them.
bool np_presentation_bind_callbacks(struct np_surface *surface, uint32_t presentation_id) {
	bool bound = false;
	struct np_frame_callback *callback;
	wl_list_for_each(callback, &surface->pending_frame_callbacks, link) {
		if (callback->presentation_id) continue;
		callback->presentation_id = presentation_id;
		bound = true;
	}
	return bound;
}

void np_presentation_rebind_callbacks(struct np_surface *surface,
	                                  uint32_t from, uint32_t to) {
	if (!from || !to || from == to) return;
	struct np_frame_callback *callback;
	wl_list_for_each(callback, &surface->pending_frame_callbacks, link) {
		if (callback->presentation_id == from)
			callback->presentation_id = to;
	}
}

static void complete_presentation(struct np_surface *surface, uint32_t presentation_id) {
	struct np_frame_callback *callback, *tmp;
	wl_list_for_each_safe(callback, tmp, &surface->pending_frame_callbacks, link) {
		if (callback->presentation_id == presentation_id)
			frame_callback_fire(callback);
	}
}

static void release_presentation(struct np_server *server,
	                             struct np_surface *owner,
	                             uint32_t presentation_id)
{
	struct np_surface *root = np_scene_root(owner);
	np_scene_presented(root ? root : owner, presentation_id);
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		np_surface_apply_unblocked(surface);
	}
}

void np_presentation_process_presented(struct np_server *server,
	                                uint32_t surface_id,
	                                uint32_t presentation_id)
{
	np_perf_count(NP_PERF_PRESENTED);
	struct np_surface *owner = np_surface_by_id(server, surface_id);
	if (!owner || !presentation_id) return;

	/* One window scene may contain commits and callbacks from several
	 * synchronized surfaces. They all become visible at this one latch. */
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		complete_presentation(surface, presentation_id);
		if (surface->fifo_barrier_active &&
		    surface->fifo_barrier_presentation_id == presentation_id) {
			surface->fifo_barrier_active = false;
			surface->fifo_barrier_presentation_id = 0;
		}
		np_surface_apply_unblocked(surface);
	}
}

void np_presentation_process_released(struct np_server *server,
	                               uint32_t surface_id,
	                               uint32_t presentation_id)
{
	struct np_surface *owner = np_surface_by_id(server, surface_id);
	if (owner && presentation_id)
		release_presentation(server, owner, presentation_id);
}

void np_presentation_finish_feedback(struct np_server *server)
{
	/* Clients cannot run until this host message handler returns, so flushing
	 * once after a binary batch preserves semantics and coalesces scene work. */
	np_presentation_flush(server);
	wl_display_flush_clients(server->display);
}

/// Publish one atomic layer snapshot per xdg window. Guest Wayland state is
/// fully resolved, but pixels remain in the original GPU resources.
void np_presentation_flush(struct np_server *server) {
	/* A scene owns client buffers until feedback returns. Never publish into a
	 * partial multi-port session that cannot return both latch and release. */
	if (!server->host_session_ready) return;
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (!surface->scene_dirty || np_scene_root(surface) != surface)
			continue;
		if (!np_backend_connected(server)) continue;
		/* A root detach has no pixels to publish. Older queued state can
		 * still reach this point (or the client can detach while a scene is
		 * dirty), so retire the impossible scene rather than retrying it on
		 * every future window's frame. */
		if (!surface->has_published) {
			uint32_t presentation_id = surface->scene_presentation_id;
			surface->scene_dirty = false;
			surface->scene_presentation_id = 0;
			np_presentation_request_refresh(surface, presentation_id);
			continue;
		}
		struct np_scene_packet packet;
		int wait_fd = -1;
		enum np_scene_build_result result = np_scene_build(
			surface, surface->scene_presentation_id, &packet, &wait_fd);
		if (result == NP_SCENE_WAIT) {
			if (!np_surface_watch_wait_fd(server, wait_fd, &surface->scene_wait_source,
			                   &surface->scene_wait_fd))
				np_surface_schedule_retry(server);
			continue;
		}
		np_presentation_clear_scene_wait(surface);
		if (result != NP_SCENE_READY) {
			surface->scene_dirty = false;
			surface->scene_presentation_id = 0;
			struct wl_client *client = wl_resource_get_client(surface->resource);
			if (result == NP_SCENE_NO_MEMORY)
				wl_client_post_no_memory(client);
			else
				wl_client_post_implementation_error(
					client, "could not construct a valid NativePipe scene");
			continue;
		}
		if (!np_backend_send_binary(server, packet.data, packet.size)) {
			np_scene_presented(surface, surface->scene_presentation_id);
			free(packet.data);
			continue;
		}
		free(packet.data);
		np_scene_damage_sent(surface);
		np_perf_count(NP_PERF_COMMIT_SENT);
		surface->scene_dirty = false;
		surface->scene_presentation_id = 0;
	}

	/* Unroled surfaces (cursor and drag icon) and the remote encoder retain the
	 * ordinary one-surface frame path. */
	wl_list_for_each(surface, &server->surfaces, link) {
		if (!surface->pending_frame) continue;
		if (np_scene_root(surface)) continue;
		unsigned char *body = surface->pending_frame;
		size_t body_size = surface->pending_frame_size;
		uint32_t presentation_id = surface->pending_presentation_id;
		surface->pending_frame = NULL;
		surface->pending_frame_size = 0;
		surface->pending_presentation_id = 0;
		if (!np_backend_send_binary(server, body, body_size))
			np_scene_presented(surface, presentation_id);
		free(body);
	}
	// Subsurface positions are guest scene state. Consume their dirty marker;
	// the host deliberately has no per-child transform to update.
	wl_list_for_each(surface, &server->surfaces, link) {
		if (!surface->host_sub_position_dirty) continue;
		surface->host_sub_position_dirty = false;
	}
}

/// A direct GPU source cannot be written while Metal is sampling it. A commit
/// that reuses such a buffer remains in protocol order until frameReleased.
/// wl_shm is compositor-copied and therefore uses copy-on-write instead.

bool np_presentation_has_unbound_callbacks(struct np_surface *surface) {
	struct np_frame_callback *callback;
	wl_list_for_each(callback, &surface->pending_frame_callbacks, link) {
		if (!callback->presentation_id) return true;
	}
	return false;
}


static void current_buffer_destroyed(struct wl_listener *listener, void *data) {
	(void)data;
	struct np_surface *surface =
		wl_container_of(listener, surface, current_buffer_destroy);
	surface->current_buffer = NULL;
	wl_list_remove(&listener->link);
	wl_list_init(&listener->link);
}

void np_presentation_set_current_buffer(struct np_surface *surface,
	                           struct wl_resource *buffer,
	                           struct np_gpu_buffer *gpu_buffer,
	                           struct np_sync_point *release_point) {
	struct np_gpu_buffer *next_gpu = gpu_buffer ? gpu_buffer : np_gpu_buffer_get(buffer);
	if (!release_point && surface->current_buffer == buffer &&
	    surface->current_gpu == next_gpu && (buffer || next_gpu)) return;
	if (!wl_list_empty(&surface->current_buffer_destroy.link))
		wl_list_remove(&surface->current_buffer_destroy.link);
	wl_list_init(&surface->current_buffer_destroy.link);
	if (surface->current_gpu) {
		if (surface->current_release_point) {
			np_gpu_buffer_queue_release(
				surface->current_gpu, surface->current_release_point);
			surface->current_release_point = NULL;
		}
		np_gpu_buffer_release_current(surface->current_gpu);
		surface->current_gpu = NULL;
	}
	if (surface->current_release_point) {
		/* Explicit sync is rejected for shm, but keep destruction robust if a
		 * partially failed commit left a point behind. */
		np_sync_point_signal(surface->current_release_point);
		surface->current_release_point = NULL;
	}
	if (surface->current_shm) {
		np_shm_texture_unref(surface->current_shm);
		surface->current_shm = NULL;
	}
	surface->current_buffer = buffer;
	if (buffer) {
		surface->current_buffer_destroy.notify = current_buffer_destroyed;
		wl_resource_add_destroy_listener(buffer, &surface->current_buffer_destroy);
	}
	surface->current_gpu = next_gpu;
	if (surface->current_gpu) {
		np_gpu_buffer_acquire_current(surface->current_gpu);
		surface->current_release_point = release_point;
	} else if (release_point) {
		np_sync_point_signal(release_point);
	}
}

/* A scene update is a latest-value marker, not a host frame. np_presentation_flush
 * consumes it after all synchronized child commits have become current and
 * emits exactly one window-level scene texture for the root. */
void np_presentation_queue_scene(struct np_surface *surface,
	                           uint32_t presentation_id) {
	if (!presentation_id) return;
	struct np_surface *root = np_scene_root(surface);
	if (!root) return;
	if (!root->scene_dirty) {
		root->scene_dirty = true;
		root->scene_presentation_id = np_presentation_next_id(surface->server);
	}
	uint32_t scene_id = root->scene_presentation_id;
	np_presentation_rebind_callbacks(surface, presentation_id, scene_id);
	if (surface->fifo_barrier_presentation_id == presentation_id)
		surface->fifo_barrier_presentation_id = scene_id;
}


void np_presentation_request_refresh(struct np_surface *surface, uint32_t presentation_id) {
	if (!presentation_id) return;
	struct np_surface *owner = np_scene_root(surface);
	if (!owner) owner = surface;
	uint32_t fields[] = {owner->id, presentation_id};
	np_window_event_send(
		surface->server, NP_GUEST_FRAME_CALLBACK_REQUESTED, fields, 2);
}

void np_presentation_add_viewport(struct np_surface *surface,
	                              struct np_window_frame *frame) {
	if (surface->viewport_state.source_set) {
		frame->has_viewport_source = true;
		frame->viewport_source.x =
			wl_fixed_to_double(surface->viewport_state.source_x);
		frame->viewport_source.y =
			wl_fixed_to_double(surface->viewport_state.source_y);
		frame->viewport_source.width =
			wl_fixed_to_double(surface->viewport_state.source_width);
		frame->viewport_source.height =
			wl_fixed_to_double(surface->viewport_state.source_height);
	}
	if (surface->viewport_state.destination_set) {
		frame->has_viewport_destination = true;
		frame->viewport_width = surface->viewport_state.destination_width;
		frame->viewport_height = surface->viewport_state.destination_height;
	}
}

/// A viewport-only commit changes how the already attached buffer is sampled.
/// Re-send that buffer's description instead of acknowledging only its frame
/// callback; the host needs the new source/destination even though the pixels
/// and resource identity did not change.
bool np_presentation_queue_last(struct np_surface *surface,
	                                  uint32_t presentation_id) {
	if (!surface->has_published) return false;
	if (np_scene_root(surface) && np_backend_surface_has_current(surface)) {
		np_presentation_queue_scene(surface, presentation_id);
		return true;
	}
	if (!surface->last_resource_id) return false;
	/* Cursor and drag-icon commits reuse the ordinary metadata envelope, but
	 * the named texture has exactly the same host-read lifetime as a scene
	 * layer. Viewport-only commits and reconnect replay must establish a fresh
	 * hold before the metadata crosses the channel. */
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
				"could not retain current cursor or drag buffer");
		return false;
	}
	struct np_window_frame frame = {
		.resource_id = surface->last_resource_id,
		.width = surface->last_width,
		.height = surface->last_height,
		.bytes_per_row = (int32_t)surface->last_stride,
		.scale = surface->scale,
		.format = surface->last_format &&
		          strcmp(surface->last_format, "bgrx8888") == 0
			? NP_WINDOW_BGRX8888
			: surface->last_format &&
			  strcmp(surface->last_format, "rgba8888") == 0
				? NP_WINDOW_RGBA8888 : NP_WINDOW_BGRA8888,
		.presentation_id = presentation_id,
	};
	np_backend_configure_frame(surface, &frame);
	if (surface->geometry_set) {
		frame.has_window_geometry = true;
		frame.window_geometry = (struct np_window_rect) {
			.x = surface->geometry_x,
			.y = surface->geometry_y,
			.width = surface->geometry_width,
			.height = surface->geometry_height,
		};
	}
	np_presentation_add_viewport(surface, &frame);
	unsigned char *body = NULL;
	size_t body_size = 0;
	if (!np_window_event_send_frame(
		    surface->server, surface->id, &frame, &body, &body_size)) {
		np_scene_presented(surface, presentation_id);
		return false;
	}
	if (surface->pending_frame) {
		np_presentation_rebind_callbacks(
			surface, surface->pending_presentation_id, presentation_id);
		np_scene_presented(surface, surface->pending_presentation_id);
		free(surface->pending_frame);
	}
	surface->pending_frame = body;
	surface->pending_frame_size = body_size;
	surface->pending_presentation_id = presentation_id;
	return true;
}
