// Shared Wayland compositor core (linked into vmpipe-wayland / remotepipe-wayland).
//
// This translates Wayland application windows to AppKit windows. It owns the
// protocol state machine and resolves each xdg surface tree into immutable
// metadata. The host samples the original Venus textures and performs the one
// composition pass directly into the NSWindow drawable.
//
//     wl_surface   #17  ->  NativeSurface #17
//     xdg_toplevel #23  ->  NativeWindow  #23  ->  NSWindow *
//
// wl_shm receives one compositor texture per wl_buffer; linux-dmabuf remains in
// the client's existing texture. Neither path creates a window-level scene
// image or an intermediate IOSurface.

#define _GNU_SOURCE

#include "compositor.h"
#include "compositor_internal.h"
#include "perf.h"
#include "dmabuf.h"
#include "decoration.h"
#include "hostlink.h"
#include "scale.h"
#include "scene.h"
#include "shm_texture.h"
#include "syncobj.h"
#ifndef NP_REMOTE
#include "virtio_resource.h"
#endif
#ifdef NP_REMOTE
#include "medialink.h"
#include "../encoder/encoder.h"
#endif
#include "fractional-scale-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "fifo-v1-server-protocol.h"
#include "xdg-decoration-server-protocol.h"
#include "text-input-v3-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#define COMPOSITOR_VERSION 4
#define XDG_WM_BASE_VERSION 3
#define SEAT_VERSION 5
#define OUTPUT_VERSION 2

static struct np_server *g_server;

/// Set NP_TRACE=1 to see what arrives from the host. Mouse motion is left out
/// because it would bury everything else.
static bool trace_enabled(void) {
	static int enabled = -1;
	if (enabled < 0)
		enabled = (getenv("NP_TRACE") != NULL ||
		           access("/run/nativepipe-trace", F_OK) == 0) ? 1 : 0;
	return enabled == 1;
}

#ifndef NP_REMOTE
/* Publish the display name only after every server-side endpoint is ready.
 * guestd treats this file as the session readiness record, so a partially
 * written or stale file must never be observable by an application launch. */
static bool publish_session_environment(const char *socket)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !runtime[0] || !socket || !socket[0]) return false;

	char path[1024];
	char temporary[1088];
	snprintf(path, sizeof(path), "%s/nativepipe-wayland.env", runtime);
	snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid());

	int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) {
		unlink(temporary);
		fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	}
	if (fd < 0) return false;

	FILE *env = fdopen(fd, "w");
	if (!env) {
		close(fd);
		unlink(temporary);
		return false;
	}
	bool ok = fprintf(env, "WAYLAND_DISPLAY=%s\n", socket) > 0;
	const char *bus = getenv("DBUS_SESSION_BUS_ADDRESS");
	if (ok && bus && bus[0])
		ok = fprintf(env, "DBUS_SESSION_BUS_ADDRESS=%s\n", bus) > 0;
	if (ok) ok = fflush(env) == 0;
	if (ok) ok = fsync(fd) == 0;
	if (fclose(env) != 0) ok = false;
	if (ok) ok = rename(temporary, path) == 0;
	if (!ok) unlink(temporary);
	return ok;
}

static void unpublish_session_environment(void)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !runtime[0]) return;
	char path[1024];
	snprintf(path, sizeof(path), "%s/nativepipe-wayland.env", runtime);
	if (unlink(path) < 0 && errno != ENOENT)
		fprintf(stderr, "[wayland] could not remove session readiness: %s\n",
		        strerror(errno));
}
#endif


static void flush_pending_frames(struct np_server *server);
static void apply_unblocked_updates(struct np_surface *surface);
static void apply_surface_update_now(struct np_surface_update *update);
static void request_host_refresh(struct np_surface *surface,
                                 uint32_t presentation_id);
static void publish_surface_buffer(struct np_surface *surface, struct wl_resource *buffer,
                                   uint32_t presentation_id,
                                   struct np_sync_point *release_point);
static void frame_add_viewport(struct np_surface *surface, cJSON *frame);
static bool parent_has_pending_subsurface_state(struct np_surface *parent);
static bool capture_subsurface_state(struct np_surface_update *update);
static void drop_queued_surface_references(struct np_server *server,
                                           struct np_surface *surface);

static int retry_dirty_scenes(void *data)
{
	struct np_server *server = data;
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link)
		apply_unblocked_updates(surface);
	flush_pending_frames(server);
	wl_display_flush_clients(server->display);
	return 0;
}

static void schedule_scene_retry(struct np_server *server)
{
	if (!server->scene_retry_timer) {
		struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
		server->scene_retry_timer = wl_event_loop_add_timer(
			loop, retry_dirty_scenes, server);
	}
	if (server->scene_retry_timer)
		wl_event_source_timer_update(server->scene_retry_timer, 1);
}

static void drop_stack_ops_referencing(struct np_server *server,
	                                   struct np_surface *surface)
{
	struct np_surface *owner;
	wl_list_for_each(owner, &server->surfaces, link) {
		struct np_subsurface_stack_op *op, *tmp;
		wl_list_for_each_safe(op, tmp, &owner->pending_stack_ops, link) {
			if (op->child != surface && op->sibling != surface) continue;
			wl_list_remove(&op->link);
			free(op);
		}
	}
}

static void detach_from_parent(struct np_surface *surface)
{
	if (!surface) return;
	drop_queued_surface_references(surface->server, surface);
	drop_stack_ops_referencing(surface->server, surface);
	if (surface->parent) {
		wl_list_remove(&surface->sibling_link);
		wl_list_init(&surface->sibling_link);
		surface->parent = NULL;
	}
}

static void detach_subsurface_tree_links(struct np_surface *surface)
{
	if (!surface) return;
	detach_from_parent(surface);
	struct np_surface *child, *tmp;
	wl_list_for_each_safe(child, tmp, &surface->children, sibling_link) {
		wl_list_remove(&child->sibling_link);
		wl_list_init(&child->sibling_link);
		child->parent = NULL;
	}
}
#ifdef NP_REMOTE
static void publish_frame_remote(struct np_surface *surface, struct wl_shm_buffer *shm,
                                 uint32_t presentation_id);
static void encoder_emit(void *user, const uint8_t *data, size_t size, uint64_t pts_ns,
                         uint16_t bitstream_epoch, uint16_t width, uint16_t height);
static uint64_t monotonic_ns(void);
static int media_listener_readable(int fd, uint32_t mask, void *data);
#endif
/// Shared by every per-client input binding, data devices included.
static void input_resource_destroy(struct wl_resource *resource);
// A popup takes keyboard focus when it grabs, which is well before the input
// code that owns focus appears below.
static void set_keyboard_focus(struct np_server *server, uint32_t window_id);
static bool same_client(struct wl_resource *a, struct wl_resource *b);
static struct np_surface *surface_by_window(struct np_server *server, uint32_t window_id);
static struct np_surface *surface_by_id(struct np_server *server, uint32_t surface_id);
static void finish_host_toplevel_configure(struct np_surface *surface,
	                                       uint32_t serial);
static void clear_pointer_focus_for_drag(struct np_server *server);
static void restore_pointer_focus_after_drag(struct np_server *server,
                                             struct np_surface *surface);

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/// wl_fixed_t is 24.8 fixed point. Pointer coordinates are surface-local
/// logical units, the same space configure speaks — not buffer pixels.
static wl_fixed_t fixed_from(double value) {
	return wl_fixed_from_double(value);
}

static void box_clear(struct np_box *box) {
	box->x = box->y = box->width = box->height = 0;
}

static void box_union(struct np_box *box, int32_t x, int32_t y, int32_t w, int32_t h) {
	if (w <= 0 || h <= 0) return;
	if (box->width == 0 || box->height == 0) {
		box->x = x; box->y = y; box->width = w; box->height = h;
		return;
	}
	int32_t left = box->x < x ? box->x : x;
	int32_t top = box->y < y ? box->y : y;
	int32_t right = (box->x + box->width) > (x + w) ? (box->x + box->width) : (x + w);
	int32_t bottom = (box->y + box->height) > (y + h) ? (box->y + box->height) : (y + h);
	box->x = left; box->y = top; box->width = right - left; box->height = bottom - top;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static struct np_surface *surface_by_window(struct np_server *server, uint32_t window_id) {
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if ((surface->toplevel || surface->popup) && surface->window_id == window_id) {
			return surface;
		}
	}
	return NULL;
}

static struct np_surface *surface_by_id(struct np_server *server, uint32_t surface_id) {
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->id == surface_id) return surface;
	}
	return NULL;
}

static cJSON *object_with_u32(const char *key, uint32_t value) {
	cJSON *object = cJSON_CreateObject();
	cJSON_AddNumberToObject(object, key, value);
	return object;
}

// ---------------------------------------------------------------------------
// wl_surface
// ---------------------------------------------------------------------------

static void surface_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *buffer, int32_t x, int32_t y) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->pending_buffer = buffer;
	surface->pending_buffer_set = true;
	if (trace_enabled()) {
		fprintf(stderr, "[wayland] attach surface=%u buffer=%s\n",
		        surface->id, buffer ? "set" : "null");
	}
}

/// Surface-local damage. Scaled to buffer pixels on commit, where the buffer
/// scale is known.
static void surface_damage(struct wl_client *client, struct wl_resource *resource,
                           int32_t x, int32_t y, int32_t width, int32_t height) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	int scale = surface->pending_scale > 0 ? surface->pending_scale : 1;
	int32_t buffer_width = surface->last_width;
	int32_t buffer_height = surface->last_height;
	if (surface->pending_buffer) {
		struct wl_shm_buffer *shm = wl_shm_buffer_get(surface->pending_buffer);
		struct np_gpu_buffer *gpu = np_gpu_buffer_get(surface->pending_buffer);
		if (shm) {
			buffer_width = wl_shm_buffer_get_width(shm);
			buffer_height = wl_shm_buffer_get_height(shm);
		} else if (gpu) {
			buffer_width = gpu->width;
			buffer_height = gpu->height;
		}
	}
	int32_t bx, by, bw, bh;
	if (buffer_width > 0 && buffer_height > 0 &&
	    np_scale_damage_to_buffer(surface->pending_transform,
	                              (uint32_t)buffer_width, (uint32_t)buffer_height,
	                              scale, x, y, width, height,
	                              &bx, &by, &bw, &bh))
		box_union(&surface->pending, bx, by, bw, bh);
	else
		box_union(&surface->pending, x * scale, y * scale, width * scale, height * scale);
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
	wl_callback_send_done(callback->resource, now_ms());
	wl_resource_destroy(callback->resource);
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
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

static uint32_t next_presentation_id(struct np_server *server) {
	uint32_t id = ++server->next_presentation_id;
	if (!id) id = ++server->next_presentation_id;
	return id;
}

/// Associate only the callbacks still in pending surface state with this
/// commit. Older callbacks retain the presentation which already owns them.
static bool bind_frame_callbacks(struct np_surface *surface, uint32_t presentation_id) {
	bool bound = false;
	struct np_frame_callback *callback;
	wl_list_for_each(callback, &surface->pending_frame_callbacks, link) {
		if (callback->presentation_id) continue;
		callback->presentation_id = presentation_id;
		bound = true;
	}
	return bound;
}

static void rebind_frame_callbacks(struct np_surface *surface,
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
		apply_unblocked_updates(surface);
	}
}

static void process_frame_presented(struct np_server *server,
	                                uint32_t surface_id,
	                                uint32_t presentation_id)
{
	np_perf_count(NP_PERF_PRESENTED);
	struct np_surface *owner = surface_by_id(server, surface_id);
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
		apply_unblocked_updates(surface);
	}
}

static void process_frame_released(struct np_server *server,
	                               uint32_t surface_id,
	                               uint32_t presentation_id)
{
	struct np_surface *owner = surface_by_id(server, surface_id);
	if (owner && presentation_id)
		release_presentation(server, owner, presentation_id);
}

static void finish_frame_feedback(struct np_server *server)
{
	/* Clients cannot run until this host message handler returns, so flushing
	 * once after a binary batch preserves semantics and coalesces scene work. */
	flush_pending_frames(server);
	wl_display_flush_clients(server->display);
}

static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *region) {
	(void)client;
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->pending_opaque_region_changed = true;
	surface->pending_opaque_region_set = region != NULL;
	memset(&surface->pending_opaque_region, 0, sizeof(surface->pending_opaque_region));
	if (region && !np_region_copy_resource(region, &surface->pending_opaque_region))
		surface->pending_opaque_region_set = false;
}
static void surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
                                     struct wl_resource *region) {
	(void)client;
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->pending_input_region_changed = true;
	surface->pending_input_region_set = region != NULL;
	memset(&surface->pending_input_region, 0, sizeof(surface->pending_input_region));
	if (region && !np_region_copy_resource(region, &surface->pending_input_region))
		surface->pending_input_region_set = false;
}

static void surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource,
                                         int32_t transform) {
	(void)client;
	if (transform < WL_OUTPUT_TRANSFORM_NORMAL ||
	    transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
		                       "invalid buffer transform %d", transform);
		return;
	}
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->pending_transform = transform;
	surface->pending_transform_changed = true;
}

static void surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource,
                                     int32_t scale) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->pending_scale = scale > 0 ? scale : 1;
	if (trace_enabled())
		fprintf(stderr, "[configure] surface=%u pending buffer scale=%d\n",
		        surface->id, surface->pending_scale);
}

/// Damage already in buffer pixels. This bounds the one copy: a client that
/// repaints a caret should not cost a full-surface memcpy.
static void surface_damage_buffer(struct wl_client *client, struct wl_resource *resource,
                                  int32_t x, int32_t y, int32_t width, int32_t height) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	box_union(&surface->pending, x, y, width, height);
}

static void surface_offset(struct wl_client *client, struct wl_resource *resource,
                           int32_t x, int32_t y) {}

#ifdef NP_REMOTE
static uint64_t monotonic_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void encoder_emit(void *user, const uint8_t *data, size_t size, uint64_t pts_ns,
                         uint16_t bitstream_epoch, uint16_t width, uint16_t height) {
	struct np_surface *surface = user;
	if (!surface || !surface->server) return;
	np_media_send(&surface->server->media, surface->id, width, height, pts_ns, bitstream_epoch,
	              data, (uint32_t)size);
}

static void queue_encoded_committed(struct np_surface *surface, int32_t width, int32_t height,
                                    const char *format_name, uint32_t presentation_id) {
	uint16_t epoch = surface->encoder ? np_encoder_epoch(surface->encoder) : surface->last_epoch;
	surface->last_epoch = epoch;
	cJSON *frame = cJSON_CreateObject();
	cJSON_AddNumberToObject(frame, "resourceID", surface->id);
	cJSON_AddNumberToObject(frame, "width", width);
	cJSON_AddNumberToObject(frame, "height", height);
	cJSON_AddNumberToObject(frame, "bytesPerRow", width * 4);
	cJSON_AddStringToObject(frame, "format", format_name);
	cJSON_AddStringToObject(frame, "source", "encoded");
	cJSON_AddStringToObject(frame, "codec", "h264");
	cJSON_AddNumberToObject(frame, "bitstreamEpoch", epoch);
	cJSON_AddNumberToObject(frame, "presentationID", presentation_id);
	cJSON_AddNumberToObject(frame, "scale", surface->scale);
	if (surface->geometry_set) {
		cJSON *geometry = cJSON_CreateObject();
		cJSON_AddNumberToObject(geometry, "x", surface->geometry_x);
		cJSON_AddNumberToObject(geometry, "y", surface->geometry_y);
		cJSON_AddNumberToObject(geometry, "width", surface->geometry_width);
		cJSON_AddNumberToObject(geometry, "height", surface->geometry_height);
		cJSON_AddItemToObject(frame, "windowGeometry", geometry);
	}
	frame_add_viewport(surface, frame);
	cJSON_AddItemToObject(frame, "damage", cJSON_CreateArray());

	surface->last_format = format_name;
	surface->last_width = width;
	surface->last_height = height;
	surface->last_resource_id = surface->id;
	surface->last_stride = (uint32_t)(width * 4);
	surface->last_source = "encoded";
	surface->has_published = true;

	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "surface", surface->id);
	cJSON_AddItemToObject(body, "frame", frame);
	if (surface->pending_frame) cJSON_Delete(surface->pending_frame);
	surface->pending_frame = body;
	surface->pending_presentation_id = presentation_id;
}

/// Remote path: encode wl_shm pixels to H.264 and ship NPEN on the media port.
static void publish_frame_remote(struct np_surface *surface, struct wl_shm_buffer *shm,
                                 uint32_t presentation_id) {
	wl_shm_buffer_begin_access(shm);
	const unsigned char *source = wl_shm_buffer_get_data(shm);
	int32_t width = wl_shm_buffer_get_width(shm);
	int32_t height = wl_shm_buffer_get_height(shm);
	int32_t stride = wl_shm_buffer_get_stride(shm);
	uint32_t format = wl_shm_buffer_get_format(shm);
	if (!source || width < 2 || height < 2) {
		wl_shm_buffer_end_access(shm);
		return;
	}

	const char *format_name = format == WL_SHM_FORMAT_ARGB8888 ? "bgra8888"
		: format == WL_SHM_FORMAT_XRGB8888 ? "bgrx8888"
		: "rgba8888";

	if (!surface->encoder) {
		surface->encoder = np_encoder_create(width, height, encoder_emit, surface);
		if (!surface->encoder) {
			fprintf(stderr, "[wayland] encoder create failed surface=%u\n", surface->id);
			wl_shm_buffer_end_access(shm);
			return;
		}
	}

	surface->last_width = width;
	surface->last_height = height;
	uint64_t pts = monotonic_ns();
	if (!np_encoder_push_bgra(surface->encoder, source, width, height, stride, pts)) {
		fprintf(stderr, "[wayland] encode failed surface=%u\n", surface->id);
		wl_shm_buffer_end_access(shm);
		return;
	}
	wl_shm_buffer_end_access(shm);
	box_clear(&surface->pending);
	queue_encoded_committed(surface, width, height, format_name, presentation_id);
}

#endif


/// Publish one atomic layer snapshot per xdg window. Guest Wayland state is
/// fully resolved, but pixels remain in the original GPU resources.
static void flush_pending_frames(struct np_server *server) {
	/* A scene owns client buffers until feedback returns. Never publish into a
	 * partial multi-port session that cannot return both latch and release. */
	if (!server->host_session_ready) return;
	struct np_surface *surface;
#ifndef NP_REMOTE
	wl_list_for_each(surface, &server->surfaces, link) {
		if (!surface->pending_frame) continue;
		struct np_surface *root = np_scene_root(surface);
		if (!root) continue;

		if (!root->scene_presentation_id)
			root->scene_presentation_id = next_presentation_id(server);
		uint32_t old = surface->pending_presentation_id;
		uint32_t scene_id = root->scene_presentation_id;
		rebind_frame_callbacks(surface, old, scene_id);
		if (surface->fifo_barrier_presentation_id == old)
			surface->fifo_barrier_presentation_id = scene_id;

		cJSON_Delete(surface->pending_frame);
		surface->pending_frame = NULL;
		surface->pending_presentation_id = 0;
		root->scene_dirty = true;
	}

	wl_list_for_each(surface, &server->surfaces, link) {
		if (!surface->scene_dirty || np_scene_root(surface) != surface)
			continue;
		if (!np_host_connected(&server->host)) continue;
		/* A root detach has no pixels to publish. Older queued state can
		 * still reach this point (or the client can detach while a scene is
		 * dirty), so retire the impossible scene rather than retrying it on
		 * every future window's frame. */
		if (!surface->has_published) {
			uint32_t presentation_id = surface->scene_presentation_id;
			surface->scene_dirty = false;
			surface->scene_presentation_id = 0;
			request_host_refresh(surface, presentation_id);
			continue;
		}
		struct np_scene_packet packet;
		if (!np_scene_build(surface, surface->scene_presentation_id, &packet)) {
			schedule_scene_retry(server);
			continue;
		}
		if (!np_host_send_binary(&server->host, packet.data, packet.size)) {
			np_scene_presented(surface, surface->scene_presentation_id);
			free(packet.data);
			continue;
		}
		free(packet.data);
		np_perf_count(NP_PERF_COMMIT_SENT);
		surface->scene_dirty = false;
		surface->scene_presentation_id = 0;
	}
#endif

	/* Unroled surfaces (cursor and drag icon) and the remote encoder retain the
	 * ordinary one-surface frame path. */
	wl_list_for_each(surface, &server->surfaces, link) {
		if (!surface->pending_frame) continue;
#ifndef NP_REMOTE
		if (np_scene_root(surface)) continue;
#endif
		cJSON *body = surface->pending_frame;
		surface->pending_frame = NULL;
		surface->pending_presentation_id = 0;
		np_host_send(&server->host, "committed", body);
	}
	// Subsurface positions are guest scene state. Consume their dirty marker;
	// the host deliberately has no per-child transform to update.
	wl_list_for_each(surface, &server->surfaces, link) {
		if (!surface->host_sub_position_dirty) continue;
		surface->host_sub_position_dirty = false;
	}
}

/// A direct source cannot be written while Metal is sampling it. A commit that
/// reuses such a buffer remains in protocol order until frameReleased.
static bool surface_update_can_apply(struct np_surface_update *update) {
#ifdef NP_REMOTE
	(void)update;
	return true;
#else
	struct np_surface *surface = update->surface;
	if (update->wait_fifo_barrier && surface->fifo_barrier_active)
		return false;
	if (update->acquire_point && !np_sync_point_ready(update->acquire_point)) {
		schedule_scene_retry(surface->server);
		return false;
	}
	struct np_surface_update *dependency;
	wl_list_for_each(dependency, &update->dependencies, link) {
		if (!surface_update_can_apply(dependency)) return false;
	}
	if (!update->buffer_set || !update->buffer) return true;
	if (wl_shm_buffer_get(update->buffer))
		return !np_shm_texture_is_busy(surface->server, update->buffer);
	struct np_gpu_buffer *gpu = np_gpu_buffer_get(update->buffer);
	return !gpu || !np_gpu_buffer_is_busy(gpu);
#endif
}

static bool has_unbound_frame_callbacks(struct np_surface *surface) {
	struct np_frame_callback *callback;
	wl_list_for_each(callback, &surface->pending_frame_callbacks, link) {
		if (!callback->presentation_id) return true;
	}
	return false;
}

static void update_buffer_destroyed(struct wl_listener *listener, void *data) {
	(void)data;
	struct np_surface_update *update =
		wl_container_of(listener, update, buffer_destroy);
	update->buffer = NULL;
	wl_list_remove(&listener->link);
	wl_list_init(&listener->link);
}

static void current_buffer_destroyed(struct wl_listener *listener, void *data) {
	(void)data;
	struct np_surface *surface =
		wl_container_of(listener, surface, current_buffer_destroy);
	surface->current_buffer = NULL;
	wl_list_remove(&listener->link);
	wl_list_init(&listener->link);
}

static void set_current_buffer(struct np_surface *surface,
	                           struct wl_resource *buffer,
	                           struct np_sync_point *release_point) {
	if (surface->current_buffer == buffer && buffer && !release_point) return;
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
		surface->current_gpu = np_gpu_buffer_get(buffer);
		if (surface->current_gpu) {
			np_gpu_buffer_acquire_current(surface->current_gpu);
			surface->current_release_point = release_point;
		} else if (release_point) {
			np_sync_point_signal(release_point);
		}
	} else if (release_point) {
		np_sync_point_signal(release_point);
	}
}

static void set_current_shm(struct np_surface *surface,
	                        struct np_shm_texture *texture)
{
	if (surface->current_shm == texture) return;
	if (surface->current_shm) np_shm_texture_unref(surface->current_shm);
	surface->current_shm = texture;
	if (texture) np_shm_texture_ref(texture);
}

/* Cursor and drag-icon surfaces have no xdg root, so they use the low-rate
 * committed control message. Their pixels still follow the same direct source
 * lifetime as a scene layer; this is not a second window rendering path. */
static bool queue_unroled_frame(struct np_surface *surface,
	                           struct wl_resource *buffer,
	                           uint32_t presentation_id,
	                           struct np_sync_point *release_point)
{
	struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
	struct np_gpu_buffer *gpu = np_gpu_buffer_get(buffer);
	uint32_t resource_id = 0, stride = 0, format = 0;
	int32_t width = 0, height = 0;
	const char *source = NULL;
	if (shm) {
#ifdef NP_REMOTE
		publish_frame_remote(surface, shm, presentation_id);
		wl_buffer_send_release(buffer);
		return true;
#else
		struct np_shm_texture *texture = np_shm_texture_upload(
			surface->server, buffer, &surface->pending);
		if (!texture) return false;
		set_current_buffer(surface, buffer, release_point);
		set_current_shm(surface, texture);
		resource_id = texture->image.resource_id;
		stride = texture->stride;
		width = wl_shm_buffer_get_width(shm);
		height = wl_shm_buffer_get_height(shm);
		format = wl_shm_buffer_get_format(shm);
		source = "cpu";
		/* The guest copy is complete; subsequent host access targets texture. */
		wl_buffer_send_release(buffer);
#endif
	} else if (gpu) {
		set_current_buffer(surface, buffer, release_point);
		resource_id = gpu->resource_id;
		stride = (uint32_t)gpu->stride;
		width = gpu->width;
		height = gpu->height;
		format = gpu->format;
		source = "gpu";
	} else {
		return false;
	}
	if (!np_scene_hold_current(surface, presentation_id)) return false;

	const char *format_name = format == WL_SHM_FORMAT_XRGB8888 ||
	                          format == 0x34325258u ? "bgrx8888" :
	                          format == 0x34324241u ||
	                          format == 0x34324258u ? "rgba8888" : "bgra8888";
	cJSON *frame = cJSON_CreateObject();
	cJSON_AddNumberToObject(frame, "resourceID", resource_id);
	cJSON_AddNumberToObject(frame, "width", width);
	cJSON_AddNumberToObject(frame, "height", height);
	cJSON_AddNumberToObject(frame, "bytesPerRow", stride);
	cJSON_AddStringToObject(frame, "format", format_name);
	cJSON_AddStringToObject(frame, "source", source);
	cJSON_AddNumberToObject(frame, "scale", surface->scale);
	cJSON_AddNumberToObject(frame, "presentationID", presentation_id);
	frame_add_viewport(surface, frame);
	cJSON_AddItemToObject(frame, "damage", cJSON_CreateArray());
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "surface", surface->id);
	cJSON_AddItemToObject(body, "frame", frame);
	if (surface->pending_frame) {
		uint32_t old = surface->pending_presentation_id;
		rebind_frame_callbacks(surface, old, presentation_id);
		np_scene_presented(surface, old);
		cJSON_Delete(surface->pending_frame);
	}
	surface->last_resource_id = resource_id;
	surface->last_width = width;
	surface->last_height = height;
	surface->last_stride = stride;
	surface->last_format = format_name;
	surface->last_source = source;
	surface->has_published = true;
	surface->pending_frame = body;
	surface->pending_presentation_id = presentation_id;
	box_clear(&surface->pending);
	return true;
}

/* A scene update is a latest-value marker, not a host frame. flush_pending_frames
 * consumes it after all synchronized child commits have become current and
 * emits exactly one window-level scene texture for the root. */
static void queue_scene_update(struct np_surface *surface,
	                           uint32_t presentation_id) {
	if (!presentation_id) return;
	if (surface->pending_frame) {
		rebind_frame_callbacks(surface, surface->pending_presentation_id,
		                       presentation_id);
		if (surface->fifo_barrier_presentation_id ==
		    surface->pending_presentation_id)
			surface->fifo_barrier_presentation_id = presentation_id;
		cJSON_Delete(surface->pending_frame);
	}
	surface->pending_frame = cJSON_CreateObject();
	surface->pending_presentation_id = presentation_id;
}

static void surface_update_destroy(struct np_surface_update *update, bool release_buffer) {
	if (!update) return;
	if (!wl_list_empty(&update->link)) wl_list_remove(&update->link);
	struct np_surface_update *dependency, *dependency_tmp;
	wl_list_for_each_safe(dependency, dependency_tmp, &update->dependencies, link)
		surface_update_destroy(dependency, release_buffer);
	struct np_subsurface_position_update *position, *position_tmp;
	wl_list_for_each_safe(position, position_tmp,
	                      &update->subsurface_positions, link) {
		wl_list_remove(&position->link);
		free(position);
	}
	struct np_subsurface_stack_op *op, *op_tmp;
	wl_list_for_each_safe(op, op_tmp, &update->stack_ops, link) {
		wl_list_remove(&op->link);
		free(op);
	}
	if (!wl_list_empty(&update->buffer_destroy.link))
		wl_list_remove(&update->buffer_destroy.link);
	if (release_buffer && update->buffer)
		wl_buffer_send_release(update->buffer);
	np_sync_point_destroy(update->acquire_point);
	np_sync_point_signal(update->release_point);
	free(update);
}

static void surface_update_drop_references(struct np_surface_update *update,
	                                        struct np_surface *surface)
{
	struct np_surface_update *dependency, *dependency_tmp;
	wl_list_for_each_safe(dependency, dependency_tmp, &update->dependencies, link) {
		if (dependency->surface == surface) {
			surface_update_destroy(dependency, true);
			continue;
		}
		surface_update_drop_references(dependency, surface);
	}
	struct np_subsurface_position_update *position, *position_tmp;
	wl_list_for_each_safe(position, position_tmp,
	                      &update->subsurface_positions, link) {
		if (position->child != surface) continue;
		wl_list_remove(&position->link);
		free(position);
	}
	struct np_subsurface_stack_op *op, *op_tmp;
	wl_list_for_each_safe(op, op_tmp, &update->stack_ops, link) {
		if (op->child != surface && op->sibling != surface) continue;
		wl_list_remove(&op->link);
		free(op);
	}
}

/* wl_subsurface.destroy takes effect immediately.  Remove references captured
 * by an older, still constrained parent CU before detaching the live tree. */
static void drop_queued_surface_references(struct np_server *server,
	                                       struct np_surface *surface)
{
	struct np_surface *owner;
	wl_list_for_each(owner, &server->surfaces, link) {
		struct np_surface_update *update;
		wl_list_for_each(update, &owner->blocked_updates, link)
			surface_update_drop_references(update, surface);
		wl_list_for_each(update, &owner->synchronized_updates, link)
			surface_update_drop_references(update, surface);
	}
}

/* Desynchronized state is effective only when no synchronized ancestor still
 * latches this surface tree. This matters for nested subsurfaces. */
static bool subsurface_is_synchronized(struct np_surface *surface) {
	for (struct np_surface *current = surface;
	     current && current->subsurface; current = current->parent) {
		if (current->sync) return true;
	}
	return false;
}

static struct np_surface_update *snapshot_surface_update(struct np_surface *surface) {
	bool callbacks = has_unbound_frame_callbacks(surface);
	bool scale_changed = surface->pending_scale != surface->scale;
	bool child_position_changed = parent_has_pending_subsurface_state(surface);
	bool synchronized_children = false;
	struct np_surface *child;
	wl_list_for_each(child, &surface->children, sibling_link) {
		if (!wl_list_empty(&child->synchronized_updates)) {
			synchronized_children = true;
			break;
		}
	}
	bool needs_refresh = surface->pending_buffer_set || callbacks ||
	                     surface->pending_fifo_set_barrier ||
	                     surface->pending_fifo_wait_barrier ||
	                     surface->pending_viewport_changed ||
	                     surface->pending_transform_changed ||
	                     surface->pending_input_region_changed ||
	                     surface->pending_opaque_region_changed ||
	                     np_syncobj_has_pending(surface) ||
	                     surface->pending_geometry_set || scale_changed ||
	                     child_position_changed || synchronized_children;
	if (!needs_refresh && !surface->pending_geometry_set &&
	    !scale_changed && !child_position_changed &&
	    !surface->host_configure_acked)
		return NULL;

	struct np_surface_update *update = calloc(1, sizeof(*update));
	if (!update) return NULL;
	wl_list_init(&update->link);
	wl_list_init(&update->dependencies);
	wl_list_init(&update->subsurface_positions);
	wl_list_init(&update->stack_ops);
	wl_list_init(&update->buffer_destroy.link);
	update->surface = surface;
	update->buffer = surface->pending_buffer;
	update->buffer_set = surface->pending_buffer_set;
	update->scale = surface->pending_scale;
	update->geometry_set = surface->pending_geometry_set;
	update->geometry_x = surface->pending_geometry_x;
	update->geometry_y = surface->pending_geometry_y;
	update->geometry_width = surface->pending_geometry_width;
	update->geometry_height = surface->pending_geometry_height;
	update->viewport_changed = surface->pending_viewport_changed;
	update->viewport = surface->pending_viewport;
	update->transform_changed = surface->pending_transform_changed;
	update->transform = surface->pending_transform;
	update->input_region_changed = surface->pending_input_region_changed;
	update->input_region_set = surface->pending_input_region_set;
	update->input_region = surface->pending_input_region;
	update->opaque_region_changed = surface->pending_opaque_region_changed;
	update->opaque_region_set = surface->pending_opaque_region_set;
	update->opaque_region = surface->pending_opaque_region;
	update->damage = surface->pending;
	update->set_fifo_barrier = surface->pending_fifo_set_barrier;
	update->wait_fifo_barrier = surface->pending_fifo_wait_barrier;
	if (!np_syncobj_take_commit(
			surface, update->buffer_set, update->buffer,
			&update->acquire_point, &update->release_point)) {
		surface_update_destroy(update, false);
		return NULL;
	}
	if (!capture_subsurface_state(update)) {
		surface_update_destroy(update, false);
		wl_client_post_no_memory(wl_resource_get_client(surface->resource));
		return NULL;
	}
	if (update->buffer) {
		update->buffer_destroy.notify = update_buffer_destroyed;
		wl_resource_add_destroy_listener(update->buffer, &update->buffer_destroy);
	}

	surface->pending_buffer = NULL;
	surface->pending_buffer_set = false;
	surface->pending_geometry_set = false;
	surface->pending_viewport_changed = false;
	surface->pending_transform_changed = false;
	surface->pending_input_region_changed = false;
	surface->pending_opaque_region_changed = false;
	box_clear(&surface->pending);

	surface->pending_fifo_set_barrier = false;
	surface->pending_fifo_wait_barrier = false;
	surface->host_configure_acked = false;

	if (needs_refresh) {
		update->presentation_id = next_presentation_id(surface->server);
		bind_frame_callbacks(surface, update->presentation_id);
	}

	return update;
}

static void request_host_refresh(struct np_surface *surface, uint32_t presentation_id) {
	if (!presentation_id) return;
	struct np_surface *owner = np_scene_root(surface);
	if (!owner) owner = surface;
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "surface", owner->id);
	cJSON_AddNumberToObject(body, "presentationID", presentation_id);
	np_host_send(&surface->server->host, "frameCallbackRequested", body);
}

static void frame_add_viewport(struct np_surface *surface, cJSON *frame) {
	if (surface->viewport_state.source_set) {
		cJSON *source = cJSON_CreateObject();
		cJSON_AddNumberToObject(source, "x",
		                        wl_fixed_to_double(surface->viewport_state.source_x));
		cJSON_AddNumberToObject(source, "y",
		                        wl_fixed_to_double(surface->viewport_state.source_y));
		cJSON_AddNumberToObject(source, "width",
		                        wl_fixed_to_double(surface->viewport_state.source_width));
		cJSON_AddNumberToObject(source, "height",
		                        wl_fixed_to_double(surface->viewport_state.source_height));
		cJSON_AddItemToObject(frame, "viewportSource", source);
	}
	if (surface->viewport_state.destination_set) {
		cJSON *destination = cJSON_CreateObject();
		cJSON_AddNumberToObject(destination, "width",
		                        surface->viewport_state.destination_width);
		cJSON_AddNumberToObject(destination, "height",
		                        surface->viewport_state.destination_height);
		cJSON_AddItemToObject(frame, "viewportDestination", destination);
	}
}

/// A viewport-only commit changes how the already attached buffer is sampled.
/// Re-send that buffer's description instead of acknowledging only its frame
/// callback; the host needs the new source/destination even though the pixels
/// and resource identity did not change.
static bool queue_last_published_frame(struct np_surface *surface,
	                                  uint32_t presentation_id) {
	if (!surface->has_published) return false;
	if (np_scene_root(surface) &&
	    (surface->current_gpu || surface->current_shm)) {
		queue_scene_update(surface, presentation_id);
		return true;
	}
	if (!surface->last_resource_id) return false;
#ifndef NP_REMOTE
	/* Cursor and drag-icon commits reuse the ordinary metadata envelope, but
	 * the named texture has exactly the same host-read lifetime as a scene
	 * layer. Viewport-only commits and reconnect replay must establish a fresh
	 * hold before the metadata crosses the channel. */
	if (!np_scene_hold_current(surface, presentation_id)) return false;
#endif
	cJSON *frame = cJSON_CreateObject();
	cJSON_AddNumberToObject(frame, "resourceID", surface->last_resource_id);
	cJSON_AddNumberToObject(frame, "width", surface->last_width);
	cJSON_AddNumberToObject(frame, "height", surface->last_height);
	cJSON_AddNumberToObject(frame, "bytesPerRow", surface->last_stride);
	cJSON_AddStringToObject(frame, "format", surface->last_format);
	if (surface->last_source)
		cJSON_AddStringToObject(frame, "source", surface->last_source);
	cJSON_AddNumberToObject(frame, "scale", surface->scale);
	cJSON_AddNumberToObject(frame, "presentationID", presentation_id);
	if (surface->geometry_set) {
		cJSON *geometry = cJSON_CreateObject();
		cJSON_AddNumberToObject(geometry, "x", surface->geometry_x);
		cJSON_AddNumberToObject(geometry, "y", surface->geometry_y);
		cJSON_AddNumberToObject(geometry, "width", surface->geometry_width);
		cJSON_AddNumberToObject(geometry, "height", surface->geometry_height);
		cJSON_AddItemToObject(frame, "windowGeometry", geometry);
	}
	frame_add_viewport(surface, frame);
	cJSON_AddItemToObject(frame, "damage", cJSON_CreateArray());

	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "surface", surface->id);
	cJSON_AddItemToObject(body, "frame", frame);
	if (surface->pending_frame) {
		rebind_frame_callbacks(surface, surface->pending_presentation_id,
		                       presentation_id);
#ifndef NP_REMOTE
		np_scene_presented(surface, surface->pending_presentation_id);
#endif
		cJSON_Delete(surface->pending_frame);
	}
	surface->pending_frame = body;
	surface->pending_presentation_id = presentation_id;
	return true;
}

static bool parent_has_pending_subsurface_state(struct np_surface *parent) {
	if (!wl_list_empty(&parent->pending_stack_ops)) return true;
	struct np_surface *child;
	wl_list_for_each(child, &parent->children, sibling_link) {
		if (child->pending_sub_position_set) return true;
	}
	return false;
}

/* Capture exactly the child state visible at this parent commit.  Moving the
 * existing synchronized updates into the CU preserves the protocol dependency
 * boundary: a later child commit cannot leak into an older parent commit. */
static bool capture_subsurface_state(struct np_surface_update *update)
{
	struct np_surface *parent = update->surface;
	struct wl_list positions;
	wl_list_init(&positions);
	struct np_surface *child;
	wl_list_for_each(child, &parent->children, sibling_link) {
		if (!child->pending_sub_position_set) continue;
		struct np_subsurface_position_update *position =
			calloc(1, sizeof(*position));
		if (!position) {
			struct np_subsurface_position_update *item, *tmp;
			wl_list_for_each_safe(item, tmp, &positions, link) {
				wl_list_remove(&item->link);
				free(item);
			}
			return false;
		}
		position->child = child;
		position->x = child->pending_sub_x;
		position->y = child->pending_sub_y;
		wl_list_insert(positions.prev, &position->link);
	}

	struct np_subsurface_position_update *position;
	wl_list_for_each(position, &positions, link) {
		position->child->pending_sub_position_set = false;
	}
	while (!wl_list_empty(&positions)) {
		position = wl_container_of(positions.next, position, link);
		wl_list_remove(&position->link);
		wl_list_insert(update->subsurface_positions.prev, &position->link);
		update->subsurface_state_changed = true;
	}
	while (!wl_list_empty(&parent->pending_stack_ops)) {
		struct np_subsurface_stack_op *op = wl_container_of(
			parent->pending_stack_ops.next, op, link);
		wl_list_remove(&op->link);
		wl_list_insert(update->stack_ops.prev, &op->link);
		update->subsurface_state_changed = true;
	}
	wl_list_for_each(child, &parent->children, sibling_link) {
		while (!wl_list_empty(&child->synchronized_updates)) {
			struct np_surface_update *dependency = wl_container_of(
				child->synchronized_updates.next, dependency, link);
			wl_list_remove(&dependency->link);
			wl_list_insert(update->dependencies.prev, &dependency->link);
		}
	}
	return true;
}

static void restack_subsurface(struct np_surface *child,
	                          struct np_surface *sibling, bool above)
{
	struct np_surface *parent = child->parent;
	if (!parent || !sibling) return;
	wl_list_remove(&child->sibling_link);
	if (sibling == parent) {
		/* Just above/below the parent is the boundary between the two groups. */
		struct wl_list *boundary = &parent->children;
		struct np_surface *candidate;
		wl_list_for_each(candidate, &parent->children, sibling_link) {
			if (candidate->above_parent) {
				boundary = &candidate->sibling_link;
				break;
			}
		}
		wl_list_insert(boundary->prev, &child->sibling_link);
		child->above_parent = above;
		return;
	}
	child->above_parent = sibling->above_parent;
	if (above)
		wl_list_insert(&sibling->sibling_link, &child->sibling_link);
	else
		wl_list_insert(sibling->sibling_link.prev, &child->sibling_link);
}

/// Position and z-order are double-buffered state of the parent. Applying
/// either in the request handler races ahead of the parent's buffer commit.
static void apply_surface_update_now(struct np_surface_update *update) {
	if (!update) return;
	while (!wl_list_empty(&update->dependencies)) {
		struct np_surface_update *dependency = wl_container_of(
			update->dependencies.next, dependency, link);
		wl_list_remove(&dependency->link);
		wl_list_init(&dependency->link);
		apply_surface_update_now(dependency);
	}
	struct np_surface *surface = update->surface;
	np_sync_point_destroy(update->acquire_point);
	update->acquire_point = NULL;
	bool scale_changed = surface->scale != update->scale;
	surface->scale = update->scale;
	if (update->geometry_set) {
		surface->geometry_set = true;
		surface->geometry_x = update->geometry_x;
		surface->geometry_y = update->geometry_y;
		surface->geometry_width = update->geometry_width;
		surface->geometry_height = update->geometry_height;
	}
	if (update->viewport_changed)
		surface->viewport_state = update->viewport;
	if (update->transform_changed)
		surface->transform = update->transform;
	if (update->input_region_changed) {
		surface->input_region_set = update->input_region_set;
		surface->input_region = update->input_region;
	}
	if (update->opaque_region_changed) {
		surface->opaque_region_set = update->opaque_region_set;
		surface->opaque_region = update->opaque_region;
	}
	struct np_subsurface_position_update *position, *position_tmp;
	wl_list_for_each_safe(position, position_tmp,
	                      &update->subsurface_positions, link) {
		position->child->sub_x = position->x;
		position->child->sub_y = position->y;
		position->child->host_sub_position_dirty = true;
		wl_list_remove(&position->link);
		free(position);
	}
	struct np_subsurface_stack_op *op, *op_tmp;
	wl_list_for_each_safe(op, op_tmp, &update->stack_ops, link) {
		restack_subsurface(op->child, op->sibling, op->above);
		wl_list_remove(&op->link);
		free(op);
	}
	surface->pending = update->damage;

	if (trace_enabled()) {
		fprintf(stderr, "[wayland] apply commit surface=%u buffer=%s present=%u%s\n",
		        surface->id, update->buffer_set ? (update->buffer ? "set" : "gone") : "unchanged",
		        update->presentation_id, update->set_fifo_barrier ? " fifo" : "");
	}

	if (update->buffer_set) {
		if (np_scene_root(surface)) {
			publish_surface_buffer(surface, update->buffer, update->presentation_id,
			                       update->release_point);
			update->release_point = NULL;
		} else if (update->buffer) {
			publish_surface_buffer(surface, update->buffer, update->presentation_id,
			                       update->release_point);
			update->release_point = NULL;
		} else if (update->presentation_id) {
			request_host_refresh(surface, update->presentation_id);
		}
	} else if (update->presentation_id) {
		if (!((update->viewport_changed || update->geometry_set || scale_changed ||
		       update->subsurface_state_changed) &&
		      queue_last_published_frame(surface, update->presentation_id)))
			request_host_refresh(surface, update->presentation_id);
	}

	if (update->set_fifo_barrier) {
		surface->fifo_barrier_active = true;
		surface->fifo_barrier_presentation_id = update->presentation_id;
	}
	surface_update_destroy(update, false);
}

static void apply_surface_update(struct np_surface_update *update) {
	if (!update) return;
	if (subsurface_is_synchronized(update->surface)) {
		wl_list_insert(update->surface->synchronized_updates.prev, &update->link);
		return;
	}
	if (!wl_list_empty(&update->surface->blocked_updates) ||
	    !surface_update_can_apply(update)) {
		wl_list_insert(update->surface->blocked_updates.prev, &update->link);
		return;
	}
	apply_surface_update_now(update);
}

static void apply_unblocked_updates(struct np_surface *surface) {
	while (!wl_list_empty(&surface->blocked_updates)) {
		struct np_surface_update *update =
			wl_container_of(surface->blocked_updates.next, update, link);
		if (!surface_update_can_apply(update)) break;
		wl_list_remove(&update->link);
		wl_list_init(&update->link);
		apply_surface_update_now(update);
	}
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
	(void)client;
	struct np_surface *surface = wl_resource_get_user_data(resource);
	/* Capture this before snapshot_surface_update() consumes the pending ack.
	 * The xdg-shell configure is complete at this wl_surface.commit boundary,
	 * even when the resulting buffer update must wait for an acquire fence or
	 * a FIFO barrier.  Waiting for snapshot_surface_update() to return an
	 * immediately applicable update accidentally coupled resize flow control
	 * back to GPU/output availability. */
	uint32_t configure_serial = surface->host_configure_acked
		? surface->host_configure_acked_serial : 0;
	struct np_surface_update *update = snapshot_surface_update(surface);
	if (configure_serial)
		finish_host_toplevel_configure(surface, configure_serial);
	if (!update) return;
	apply_surface_update(update);
}

static void publish_surface_buffer(struct np_surface *surface, struct wl_resource *buffer,
                                   uint32_t presentation_id,
                                   struct np_sync_point *release_point) {
#ifdef NP_REMOTE
	if (!buffer) {
		surface->has_published = false;
		np_sync_point_signal(release_point);
		request_host_refresh(surface, presentation_id);
		return;
	}
	struct wl_shm_buffer *remote_shm = wl_shm_buffer_get(buffer);
	if (remote_shm) {
		publish_frame_remote(surface, remote_shm, presentation_id);
		wl_buffer_send_release(buffer);
	} else {
		np_sync_point_signal(release_point);
		request_host_refresh(surface, presentation_id);
	}
	return;
#else
	struct np_surface *root = np_scene_root(surface);
	if (trace_enabled())
		fprintf(stderr,
		        "[wayland] publish surface=%u root=%u top=%d popup=%d sub=%d buffer=%s\n",
		        surface->id, root ? root->id : 0, surface->toplevel != NULL,
		        surface->popup != NULL, surface->subsurface != NULL,
		        buffer ? "set" : "gone");
	if (root) {
		if (!buffer) {
			set_current_buffer(surface, NULL, release_point);
			surface->has_published = false;
			/* A detached child changes its containing window, so the root scene
			 * must be recomposed without that child. A detached root has no scene
			 * to compose. Queueing one left scene_dirty set forever because
			 * np_scene_compose correctly rejects a root with no published buffer;
			 * every unrelated presentation then retried the impossible frame. */
			if (surface == root)
				request_host_refresh(surface, presentation_id);
			else
				queue_scene_update(surface, presentation_id);
			return;
		}

		struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
		struct np_gpu_buffer *gpu = np_gpu_buffer_get(buffer);
		if (shm) {
			struct np_shm_texture *texture = np_shm_texture_upload(
				surface->server, buffer, &surface->pending);
			if (!texture) {
				if (trace_enabled())
					fprintf(stderr, "[wayland] shm upload unavailable surface=%u\n",
					        surface->id);
				return;
			}
			set_current_buffer(surface, buffer, release_point);
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
			set_current_buffer(surface, buffer, release_point);
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
			set_current_buffer(surface, NULL, release_point);
			if (trace_enabled())
				fprintf(stderr, "[wayland] scene surface=%u: unsupported buffer\n",
				        surface->id);
			return;
		}
		if (trace_enabled()) {
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
		queue_scene_update(surface, presentation_id);
		return;
	}

	if (!buffer) return;  // client detached its unroled buffer

	/* A drag icon is commonly committed immediately before start_drag assigns
	 * its role. Publish the direct resource now and let the host retain it until
	 * dragIconChanged supplies that role. */
	if (!queue_unroled_frame(
			surface, buffer, presentation_id, release_point) && trace_enabled()) {
		fprintf(stderr, "[wayland] commit surface=%u: buffer is neither shm nor gpu\n",
		        surface->id);
	}
	if (presentation_id && surface->pending_presentation_id != presentation_id)
		request_host_refresh(surface, presentation_id);
#endif
}

static const struct wl_surface_interface surface_implementation = {
	.destroy = surface_destroy_handler,
	.attach = surface_attach,
	.damage = surface_damage,
	.frame = surface_frame,
	.set_opaque_region = surface_set_opaque_region,
	.set_input_region = surface_set_input_region,
	.commit = surface_commit,
	.set_buffer_transform = surface_set_buffer_transform,
	.set_buffer_scale = surface_set_buffer_scale,
	.damage_buffer = surface_damage_buffer,
	.offset = surface_offset,
};

// ---------------------------------------------------------------------------
// wp_fifo_manager_v1
// ---------------------------------------------------------------------------

static void fifo_resource_destroy(struct wl_resource *resource) {
	struct np_fifo *fifo = wl_resource_get_user_data(resource);
	if (!fifo) return;
	if (fifo->surface && fifo->surface->fifo == fifo)
		fifo->surface->fifo = NULL;
	free(fifo);
}

static void fifo_destroy_request(struct wl_client *client,
	                             struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static struct np_surface *fifo_surface_or_error(struct wl_resource *resource) {
	struct np_fifo *fifo = wl_resource_get_user_data(resource);
	if (fifo && fifo->surface) return fifo->surface;
	wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
	                       "the associated wl_surface was destroyed");
	return NULL;
}

static void fifo_set_barrier(struct wl_client *client,
	                         struct wl_resource *resource) {
	(void)client;
	struct np_surface *surface = fifo_surface_or_error(resource);
	if (surface) surface->pending_fifo_set_barrier = true;
}

static void fifo_wait_barrier(struct wl_client *client,
	                          struct wl_resource *resource) {
	(void)client;
	struct np_surface *surface = fifo_surface_or_error(resource);
	if (surface) surface->pending_fifo_wait_barrier = true;
}

static const struct wp_fifo_v1_interface fifo_implementation = {
	.set_barrier = fifo_set_barrier,
	.wait_barrier = fifo_wait_barrier,
	.destroy = fifo_destroy_request,
};

static void fifo_manager_destroy(struct wl_client *client,
	                             struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static void fifo_manager_get_fifo(struct wl_client *client,
	                              struct wl_resource *manager_resource,
	                              uint32_t id,
	                              struct wl_resource *surface_resource) {
	struct np_surface *surface = wl_resource_get_user_data(surface_resource);
	if (!surface) return;
	if (surface->fifo) {
		wl_resource_post_error(manager_resource,
		                       WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
		                       "a fifo object already exists for this surface");
		return;
	}

	struct np_fifo *fifo = calloc(1, sizeof(*fifo));
	if (!fifo) {
		wl_client_post_no_memory(client);
		return;
	}
	fifo->surface = surface;
	fifo->resource = wl_resource_create(client, &wp_fifo_v1_interface, 1, id);
	if (!fifo->resource) {
		free(fifo);
		wl_client_post_no_memory(client);
		return;
	}
	surface->fifo = fifo;
	wl_resource_set_implementation(fifo->resource, &fifo_implementation,
	                               fifo, fifo_resource_destroy);
}

static const struct wp_fifo_manager_v1_interface fifo_manager_implementation = {
	.destroy = fifo_manager_destroy,
	.get_fifo = fifo_manager_get_fifo,
};

static void fifo_manager_bind(struct wl_client *client, void *data,
	                          uint32_t version, uint32_t id) {
	(void)data;
	struct wl_resource *resource = wl_resource_create(
		client, &wp_fifo_manager_v1_interface, version > 1 ? 1 : version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &fifo_manager_implementation,
	                               NULL, NULL);
}

static void surface_resource_destroy(struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface) return;

	/* A client disconnect destroys all of its protocol resources, but libwayland
	 * does not promise that role objects are destroyed before wl_surface.  Every
	 * role resource stores this np_surface as user data, so detach those pointers
	 * before freeing it.  Their later destroy callbacks will then be harmless
	 * instead of reading freed memory and corrupting the next client connection. */
	if (surface->xdg_surface)
		wl_resource_set_user_data(surface->xdg_surface, NULL);
	if (surface->toplevel)
		wl_resource_set_user_data(surface->toplevel, NULL);
	if (surface->popup)
		wl_resource_set_user_data(surface->popup, NULL);
	if (surface->decoration)
		wl_resource_set_user_data(surface->decoration, NULL);
	if (surface->fractional_scale)
		wl_resource_set_user_data(surface->fractional_scale, NULL);
	if (surface->viewport)
		wl_resource_set_user_data(surface->viewport, NULL);
	if (surface->subsurface)
		wl_resource_set_user_data(surface->subsurface, NULL);

	if (surface->fifo) {
		surface->fifo->surface = NULL;
		surface->fifo = NULL;
	}
	if (surface->server->drag_icon == resource) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNullToObject(body, "surface");
		np_host_send(&surface->server->host, "dragIconChanged", body);
		surface->server->drag_icon = NULL;
	}
	if (surface->server->pointer_surface == surface->id) {
		surface->server->pointer_surface = 0;
		surface->server->pointer_window = 0;
	}
	if (surface->server->drag_focus_surface == surface->id)
		surface->server->drag_focus_surface = 0;

	// Children may outlive their parent's resource. Remove both active stacking
	// links and unapplied restack operations before any pointer can go stale.
	detach_subsurface_tree_links(surface);
	if (surface->toplevel) {
		np_host_send(&surface->server->host, "toplevelDestroyed",
		             object_with_u32("window", surface->window_id));
	}
	if (surface->popup) {
		np_host_send(&surface->server->host, "popupDestroyed",
		             object_with_u32("window", surface->window_id));
	}
	np_host_send(&surface->server->host, "surfaceDestroyed",
	             object_with_u32("surface", surface->id));
	if (surface->pending_frame) {
		cJSON_Delete(surface->pending_frame);
		surface->pending_frame = NULL;
	}
	if (surface->host_configure_idle) {
		wl_event_source_remove(surface->host_configure_idle);
		surface->host_configure_idle = NULL;
	}
	struct np_frame_callback *callback, *callback_tmp;
	wl_list_for_each_safe(callback, callback_tmp, &surface->pending_frame_callbacks, link) {
		wl_resource_destroy(callback->resource);
	}
	struct np_surface_update *update, *update_tmp;
	wl_list_for_each_safe(update, update_tmp, &surface->blocked_updates, link) {
		surface_update_destroy(update, true);
	}
	wl_list_for_each_safe(update, update_tmp, &surface->synchronized_updates, link) {
		surface_update_destroy(update, true);
	}
	set_current_buffer(surface, NULL, NULL);
	np_syncobj_surface_destroyed(surface);
#ifdef NP_REMOTE
	if (surface->encoder) {
		np_encoder_destroy(surface->encoder);
		surface->encoder = NULL;
	}
#endif
	free(surface->title);
	free(surface->app_id);
	surface->title = surface->app_id = NULL;
#ifndef NP_REMOTE
	np_scene_destroy(surface);
#endif
	wl_list_remove(&surface->link);
	free(surface);
}

// ---------------------------------------------------------------------------
// wl_data_device_manager
//
// Real clients treat this as mandatory — foot refuses to start without it — so
// a compositor that omits it is not usable, whatever else works. This carries
// the selection between guest clients; bridging it to NSPasteboard is a separate
// job on the host side.
// ---------------------------------------------------------------------------

#define NP_MAX_MIME_TYPES 24

struct np_data_source {
	struct wl_resource *resource;
	char *mime_types[NP_MAX_MIME_TYPES];
	int mime_count;
	uint32_t actions;
};

struct np_data_offer {
	struct wl_list link;
	struct np_server *server;
	struct wl_resource *resource;
	struct wl_resource *device;
	struct wl_resource *source;
	/// True when the bytes live on the macOS pasteboard, in which case `source`
	/// is NULL and a receive has to make a round trip to the host.
	bool from_host;
	bool dnd;
	bool active;
	bool dropped;
	bool finished;
	char *accepted_mime;
	uint32_t actions;
	uint32_t preferred_action;
	uint32_t chosen_action;
};

static void detach_offers_for_source(struct np_server *server,
	                                  struct wl_resource *source,
	                                  bool finished) {
	if (!server || !source) return;
	struct np_data_offer *offer;
	wl_list_for_each(offer, &server->data_offers, link) {
		if (offer->source != source) continue;
		offer->source = NULL;
		offer->active = false;
		if (finished) offer->finished = true;
	}
}

// Defined with the clipboard transport further down; the selection can change
// from here, when a source is destroyed.
static void announce_selection_to_host(struct np_server *server);

static void data_source_free(struct np_data_source *source) {
	for (int i = 0; i < source->mime_count; i++) free(source->mime_types[i]);
	free(source);
}

static void data_source_offer(struct wl_client *client, struct wl_resource *resource,
                              const char *mime_type) {
	struct np_data_source *source = wl_resource_get_user_data(resource);
	if (!source || source->mime_count >= NP_MAX_MIME_TYPES || !mime_type) return;
	source->mime_types[source->mime_count++] = strdup(mime_type);
}

static void data_source_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void data_source_set_actions(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t actions) {
	struct np_data_source *source = wl_resource_get_user_data(resource);
	if (source) source->actions = actions;
}

static const struct wl_data_source_interface data_source_implementation = {
	.offer = data_source_offer,
	.destroy = data_source_destroy_handler,
	.set_actions = data_source_set_actions,
};

static void data_source_resource_destroy(struct wl_resource *resource) {
	struct np_data_source *source = wl_resource_get_user_data(resource);
	if (g_server) {
		/* Offers outlive their source resource in the protocol. Detach the weak
		 * resource pointer before libwayland frees it so late destroy/finish
		 * requests cannot address a reused wl_resource. */
		detach_offers_for_source(g_server, resource, false);
	}
	if (g_server && g_server->selection_source == resource) {
		g_server->selection_source = NULL;
		announce_selection_to_host(g_server);
	}
	if (g_server && g_server->drag_source == resource) {
		if (g_server->drag_icon) {
			cJSON *body = cJSON_CreateObject();
			cJSON_AddNullToObject(body, "surface");
			np_host_send(&g_server->host, "dragIconChanged", body);
		}
		g_server->drag_source = NULL;
		g_server->drag_origin = NULL;
		g_server->drag_icon = NULL;
		g_server->drag_focus_window = 0;
		g_server->drag_dropped = false;
	}
	if (source) data_source_free(source);
}

/// The receiving half: a client asked for the selection in a particular type,
/// and the pipe it handed over goes straight to the owner.
static void clip_request_from_host(struct np_server *server, const char *mime_type, int fd);

static void data_offer_receive(struct wl_client *client, struct wl_resource *resource,
                               const char *mime_type, int32_t fd) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (offer && offer->from_host) {
		clip_request_from_host(offer->server, mime_type, fd);
		return;
	}
	if (offer && offer->source) {
		wl_data_source_send_send(offer->source, mime_type, fd);
	}
	close(fd);
}
static void data_offer_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void data_offer_resource_destroy(struct wl_resource *resource) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (!offer) return;
	if (offer->dnd && offer->source && !offer->finished &&
	    (offer->active || offer->dropped) &&
	    offer->server->drag_source == offer->source) {
		/* Destroying the current offer before finish is the destination's
		 * cancellation path (notably for the ASK action). Never leave the source
		 * waiting forever for dnd_finished. */
		struct wl_resource *source = offer->source;
		wl_data_source_send_cancelled(source);
		detach_offers_for_source(offer->server, source, false);
		offer->server->drag_source = NULL;
		offer->server->drag_origin = NULL;
		offer->server->drag_focus_window = 0;
		offer->server->drag_dropped = false;
		if (offer->server->drag_icon) {
			cJSON *body = cJSON_CreateObject();
			cJSON_AddNullToObject(body, "surface");
			np_host_send(&offer->server->host, "dragIconChanged", body);
			offer->server->drag_icon = NULL;
		}
	}
	wl_list_remove(&offer->link);
	free(offer->accepted_mime);
	free(offer);
}

static bool nullable_string_equal(const char *a, const char *b) {
	if (!a || !b) return a == b;
	return strcmp(a, b) == 0;
}

static void data_offer_accept(struct wl_client *client, struct wl_resource *resource,
                              uint32_t serial, const char *mime_type) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (!offer || nullable_string_equal(offer->accepted_mime, mime_type)) return;
	char *accepted = mime_type ? strdup(mime_type) : NULL;
	if (mime_type && !accepted) {
		wl_client_post_no_memory(client);
		return;
	}
	free(offer->accepted_mime);
	offer->accepted_mime = accepted;
	if (offer->source) wl_data_source_send_target(offer->source, mime_type);
}
static void data_offer_finish(struct wl_client *client, struct wl_resource *resource) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (!offer || !offer->dnd || !offer->source || !offer->dropped ||
	    !offer->accepted_mime || !offer->chosen_action || offer->finished)
		return;
	struct wl_resource *source = offer->source;
	if (wl_resource_get_version(source) >= 3) {
		wl_data_source_send_dnd_finished(source);
	}
	detach_offers_for_source(offer->server, source, true);
	if (offer->server->drag_source == source) {
		offer->server->drag_source = NULL;
		offer->server->drag_origin = NULL;
		offer->server->drag_icon = NULL;
		offer->server->drag_focus_window = 0;
		offer->server->drag_dropped = false;
	}
}
static void data_offer_set_actions(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t actions, uint32_t preferred) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (!offer || !offer->dnd || !offer->source || offer->dropped) return;
	struct np_data_source *source = wl_resource_get_user_data(offer->source);
	if (!source) return;
	offer->actions = actions;
	offer->preferred_action = preferred;
	uint32_t available = actions & source->actions;
	uint32_t chosen = (preferred && (available & preferred)) ? preferred : 0;
	if (!chosen && (available & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY))
		chosen = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	if (!chosen && (available & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE))
		chosen = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
	if (!chosen && (available & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK))
		chosen = WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
	/* GTK updates set_actions from every drag motion. Emitting the unchanged
	 * result makes its action callback send set_actions again, creating an
	 * unbounded client/server feedback loop. The protocol describes an action
	 * change notification, so silence is required when negotiation is stable. */
	if (chosen == offer->chosen_action) return;
	offer->chosen_action = chosen;
	wl_data_offer_send_action(resource, chosen);
	if (wl_resource_get_version(offer->source) >= 3) {
		wl_data_source_send_action(offer->source, chosen);
	}
}

static const struct wl_data_offer_interface data_offer_implementation = {
	.accept = data_offer_accept,
	.receive = data_offer_receive,
	.destroy = data_offer_destroy_handler,
	.finish = data_offer_finish,
	.set_actions = data_offer_set_actions,
};

static struct wl_resource *create_data_offer(struct np_server *server,
                                              struct wl_resource *device,
                                              struct wl_resource *source_resource,
                                              bool dnd) {
	struct wl_resource *offer = wl_resource_create(
		wl_resource_get_client(device), &wl_data_offer_interface,
		wl_resource_get_version(device), 0);
	if (!offer) return NULL;
	struct np_data_offer *state = calloc(1, sizeof(*state));
	if (!state) {
		wl_resource_destroy(offer);
		return NULL;
	}
	state->server = server;
	state->resource = offer;
	state->device = device;
	state->source = source_resource;
	state->dnd = dnd;
	wl_list_insert(&server->data_offers, &state->link);
	wl_resource_set_implementation(offer, &data_offer_implementation, state,
	                               data_offer_resource_destroy);
	return offer;
}

static void announce_offer(struct wl_resource *device, struct wl_resource *offer,
                           struct wl_resource *source_resource, bool dnd) {
	struct np_data_source *source = wl_resource_get_user_data(source_resource);
	if (!source) return;
	wl_data_device_send_data_offer(device, offer);
	for (int i = 0; i < source->mime_count; i++) {
		wl_data_offer_send_offer(offer, source->mime_types[i]);
	}
	if (dnd && wl_resource_get_version(offer) >= 3) {
		wl_data_offer_send_source_actions(offer, source->actions);
		wl_data_offer_send_action(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
	}
}


// ---------------------------------------------------------------------------
// Clipboard transport
//
// Wayland moves selection data over a pipe the receiver supplies, and the host
// channel carries JSON. Base64 is the bridge. It costs a third in size, which
// for clipboard payloads is worth not having to add a second binary framing and
// keep the two in step — the same reasoning that kept only pointer motion and
// scroll on the fast path.
// ---------------------------------------------------------------------------

/// A clipboard payload larger than this is refused rather than buffered. The
/// point is to bound what one paste can make the compositor hold, not to
/// support moving disk images through the selection.
#define NP_CLIP_MAX (16u * 1024u * 1024u)

static const char NP_B64_ALPHABET[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *np_base64_encode(const unsigned char *data, size_t len) {
	char *out = malloc(((len + 2) / 3) * 4 + 1);
	if (!out) return NULL;
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = data[i] << 16;
		if (i + 1 < len) v |= data[i + 1] << 8;
		if (i + 2 < len) v |= data[i + 2];
		out[o++] = NP_B64_ALPHABET[(v >> 18) & 0x3f];
		out[o++] = NP_B64_ALPHABET[(v >> 12) & 0x3f];
		out[o++] = i + 1 < len ? NP_B64_ALPHABET[(v >> 6) & 0x3f] : '=';
		out[o++] = i + 2 < len ? NP_B64_ALPHABET[v & 0x3f] : '=';
	}
	out[o] = '\0';
	return out;
}

static unsigned char *np_base64_decode(const char *text, size_t *out_len) {
	static signed char reverse[256];
	static bool built = false;
	if (!built) {
		memset(reverse, -1, sizeof(reverse));
		for (int i = 0; i < 64; i++) reverse[(unsigned char)NP_B64_ALPHABET[i]] = (signed char)i;
		built = true;
	}
	size_t len = strlen(text);
	unsigned char *out = malloc(len / 4 * 3 + 3);
	if (!out) return NULL;
	size_t o = 0;
	unsigned accumulator = 0;
	int bits = 0;
	for (size_t i = 0; i < len; i++) {
		signed char value = reverse[(unsigned char)text[i]];
		if (value < 0) continue;               // '=' and any stray whitespace
		accumulator = (accumulator << 6) | (unsigned)value;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (unsigned char)((accumulator >> bits) & 0xff);
		}
	}
	*out_len = o;
	return out;
}

/// Draining a guest client's selection into a buffer, on its way to the host.
struct np_clip_read {
	struct wl_list link;
	struct np_server *server;
	uint32_t token;
	char *mime;
	int fd;
	struct wl_event_source *source;
	unsigned char *data;
	size_t len, cap;
};

/// Pushing the macOS pasteboard's bytes into a guest client's pipe.
struct np_clip_write {
	struct wl_list link;
	int fd;
	struct wl_event_source *source;
	unsigned char *data;
	size_t len, sent;
};

static void clip_read_finish(struct np_clip_read *read_state, bool ok) {
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "token", read_state->token);
	cJSON_AddStringToObject(body, "mimeType", read_state->mime);
	char *encoded = ok ? np_base64_encode(read_state->data, read_state->len) : NULL;
	if (encoded) {
		cJSON_AddStringToObject(body, "base64", encoded);
		free(encoded);
	} else {
		cJSON_AddNullToObject(body, "base64");
	}
	np_host_send(&read_state->server->host, "selectionData", body);

	if (read_state->source) wl_event_source_remove(read_state->source);
	close(read_state->fd);
	wl_list_remove(&read_state->link);
	free(read_state->mime);
	free(read_state->data);
	free(read_state);
}

static int clip_read_ready(int fd, uint32_t mask, void *data) {
	struct np_clip_read *read_state = data;
	for (;;) {
		if (read_state->len == read_state->cap) {
			size_t cap = read_state->cap ? read_state->cap * 2 : 8192;
			if (cap > NP_CLIP_MAX) {
				fprintf(stderr, "[wayland] selection exceeds %u bytes; refusing\n",
				        NP_CLIP_MAX);
				clip_read_finish(read_state, false);
				return 0;
			}
			unsigned char *grown = realloc(read_state->data, cap);
			if (!grown) {
				clip_read_finish(read_state, false);
				return 0;
			}
			read_state->data = grown;
			read_state->cap = cap;
		}
		ssize_t got = read(fd, read_state->data + read_state->len,
		                   read_state->cap - read_state->len);
		if (got > 0) {
			read_state->len += (size_t)got;
			continue;
		}
		if (got < 0 && errno == EINTR) continue;
		if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
		// Zero is the writer closing its end, which is how a source signals that
		// it has handed over everything.
		clip_read_finish(read_state, got == 0);
		return 0;
	}
}

/// Asks the guest's current selection owner for `mime_type` and reports the
/// bytes back to the host under `token`.
static void clip_serve_host_request(struct np_server *server, uint32_t token,
                                    const char *mime_type) {
	int pipe_fds[2];
	bool failed = !server->selection_source || pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) < 0;

	struct np_clip_read *read_state = failed ? NULL : calloc(1, sizeof(*read_state));
	if (failed || !read_state) {
		if (!failed) { close(pipe_fds[0]); close(pipe_fds[1]); }
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "token", token);
		cJSON_AddStringToObject(body, "mimeType", mime_type);
		cJSON_AddNullToObject(body, "base64");
		np_host_send(&server->host, "selectionData", body);
		free(read_state);
		return;
	}

	read_state->server = server;
	read_state->token = token;
	read_state->mime = strdup(mime_type);
	read_state->fd = pipe_fds[0];
	wl_list_insert(&server->clip_reads, &read_state->link);

	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	read_state->source = wl_event_loop_add_fd(loop, pipe_fds[0],
	                                          WL_EVENT_READABLE | WL_EVENT_HANGUP,
	                                          clip_read_ready, read_state);

	// The source writes into the far end and closes it; the compositor must let
	// go of its copy or the read would never see EOF.
	wl_data_source_send_send(server->selection_source, mime_type, pipe_fds[1]);
	close(pipe_fds[1]);
	wl_display_flush_clients(server->display);
}

static void clip_write_finish(struct np_clip_write *write_state) {
	if (write_state->source) wl_event_source_remove(write_state->source);
	close(write_state->fd);
	wl_list_remove(&write_state->link);
	free(write_state->data);
	free(write_state);
}

static int clip_write_ready(int fd, uint32_t mask, void *data) {
	struct np_clip_write *write_state = data;
	while (write_state->sent < write_state->len) {
		ssize_t put = write(fd, write_state->data + write_state->sent,
		                    write_state->len - write_state->sent);
		if (put > 0) {
			write_state->sent += (size_t)put;
			continue;
		}
		if (put < 0 && errno == EINTR) continue;
		if (put < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
		break;   // the client gave up on reading; nothing left to do for it
	}
	clip_write_finish(write_state);
	return 0;
}

/// Pending pastes waiting on the host, keyed by token. A client hands over a
/// pipe and expects bytes; the answer arrives one round trip later.
struct np_clip_pending {
	struct wl_list link;
	uint32_t token;
	int fd;
};

static void clip_request_from_host(struct np_server *server, const char *mime_type, int fd) {
	struct np_clip_pending *pending = calloc(1, sizeof(*pending));
	if (!pending) {
		close(fd);
		return;
	}
	pending->token = ++server->next_clip_token;
	pending->fd = fd;
	wl_list_insert(&server->clip_writes, &pending->link);

	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "token", pending->token);
	cJSON_AddStringToObject(body, "mimeType", mime_type);
	np_host_send(&server->host, "hostSelectionRequest", body);
}

/// Completes a paste: the host answered, so the bytes go into the pipe the
/// client supplied. Nothing here blocks — an unread pipe just leaves the write
/// pending until the loop says it will take more.
static void clip_deliver_host_data(struct np_server *server, uint32_t token,
                                   const char *base64) {
	struct np_clip_pending *pending, *tmp;
	int fd = -1;
	wl_list_for_each_safe(pending, tmp, &server->clip_writes, link) {
		if (pending->token != token) continue;
		fd = pending->fd;
		wl_list_remove(&pending->link);
		free(pending);
		break;
	}
	if (fd < 0) return;

	size_t len = 0;
	unsigned char *data = base64 ? np_base64_decode(base64, &len) : NULL;
	if (!data || len == 0) {
		// Closing an empty pipe is a valid answer: the client sees EOF and
		// concludes the selection had nothing in that type.
		free(data);
		close(fd);
		return;
	}

	struct np_clip_write *write_state = calloc(1, sizeof(*write_state));
	if (!write_state) {
		free(data);
		close(fd);
		return;
	}
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	write_state->fd = fd;
	write_state->data = data;
	write_state->len = len;
	wl_list_insert(&server->clip_writes, &write_state->link);

	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	write_state->source = wl_event_loop_add_fd(loop, fd, WL_EVENT_WRITABLE,
	                                           clip_write_ready, write_state);
	clip_write_ready(fd, WL_EVENT_WRITABLE, write_state);
}

/// Tells the host what the guest's selection now offers, so it can put matching
/// types on NSPasteboard. An empty list means the guest gave the selection up.
static void announce_selection_to_host(struct np_server *server) {
	cJSON *types = cJSON_CreateArray();
	if (server->selection_source) {
		struct np_data_source *source = wl_resource_get_user_data(server->selection_source);
		if (source) {
			for (int i = 0; i < source->mime_count; i++) {
				cJSON_AddItemToArray(types, cJSON_CreateString(source->mime_types[i]));
			}
		}
	}
	cJSON *body = cJSON_CreateObject();
	cJSON_AddItemToObject(body, "mimeTypes", types);
	np_host_send(&server->host, "selectionOffered", body);
}

/// Announces the current selection to one data device: a fresh offer, its types,
/// and then the selection itself. Order matters — a client must see the offer
/// object before it is told that object is the selection.
static void send_selection_to(struct np_server *server, struct wl_resource *device) {
	if (!server->selection_source && server->host_mime_count > 0) {
		// The Mac owns the clipboard. The offer is real as far as the client is
		// concerned; only the fetch is different.
		struct wl_resource *offer = create_data_offer(server, device, NULL, false);
		if (!offer) return;
		struct np_data_offer *state = wl_resource_get_user_data(offer);
		state->from_host = true;
		wl_data_device_send_data_offer(device, offer);
		for (int i = 0; i < server->host_mime_count; i++) {
			wl_data_offer_send_offer(offer, server->host_mime[i]);
		}
		wl_data_device_send_selection(device, offer);
		return;
	}
	if (!server->selection_source) {
		wl_data_device_send_selection(device, NULL);
		return;
	}
	struct np_data_source *source = wl_resource_get_user_data(server->selection_source);
	if (!source) return;

	struct wl_resource *offer = create_data_offer(server, device,
	                                             server->selection_source, false);
	if (!offer) return;
	announce_offer(device, offer, server->selection_source, false);
	wl_data_device_send_selection(device, offer);
}

static void drag_send_leave(struct np_server *server, uint32_t window_id) {
	struct np_surface *surface = surface_by_id(server, server->drag_focus_surface);
	if (!surface) surface = surface_by_window(server, window_id);
	if (!surface) return;
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(surface->resource)) continue;
		struct np_data_offer *offer;
		wl_list_for_each(offer, &server->data_offers, link) {
			if (!offer->active || offer->device != device->resource ||
			    offer->source != server->drag_source) continue;
			offer->active = false;
			if (offer->accepted_mime) {
				free(offer->accepted_mime);
				offer->accepted_mime = NULL;
				wl_data_source_send_target(offer->source, NULL);
			}
			if (offer->chosen_action) {
				offer->chosen_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
				wl_data_offer_send_action(offer->resource,
				                          WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
				if (wl_resource_get_version(offer->source) >= 3)
					wl_data_source_send_action(offer->source,
					                           WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
			}
		}
		wl_data_device_send_leave(device->resource);
	}
	server->drag_focus_surface = 0;
}

static void drag_send_enter(struct np_server *server, struct np_surface *surface,
                            wl_fixed_t x, wl_fixed_t y) {
	if (!server->drag_source || !surface) return;
	uint32_t serial = wl_display_next_serial(server->display);
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(surface->resource)) continue;
		struct wl_resource *offer = create_data_offer(
			server, device->resource, server->drag_source, true);
		if (!offer) continue;
		struct np_data_offer *state = wl_resource_get_user_data(offer);
		if (state) state->active = true;
		announce_offer(device->resource, offer, server->drag_source, true);
		wl_data_device_send_enter(device->resource, serial, surface->resource, x, y, offer);
	}
	struct np_surface *root = np_scene_root(surface);
	server->drag_focus_window = root ? root->window_id : surface->window_id;
	server->drag_focus_surface = surface->id;
}

static void drag_send_motion(struct np_server *server, struct np_surface *surface,
                             uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(surface->resource)) continue;
		bool active = false;
		struct np_data_offer *offer;
		wl_list_for_each(offer, &server->data_offers, link) {
			if (offer->active && offer->device == device->resource &&
			    offer->source == server->drag_source) {
				active = true;
				break;
			}
		}
		if (!active) continue;
		wl_data_device_send_motion(device->resource, time, x, y);
	}
}

static void drag_finish_on_button_release(struct np_server *server) {
	if (!server->drag_source) return;
	struct np_surface *surface = surface_by_id(server, server->drag_focus_surface);
	if (!surface) surface = surface_by_window(server, server->drag_focus_window);
	if (!surface) {
		struct wl_resource *source = server->drag_source;
		wl_data_source_send_cancelled(source);
		detach_offers_for_source(server, source, false);
		server->drag_source = NULL;
		server->drag_origin = NULL;
		if (server->drag_icon) {
			cJSON *body = cJSON_CreateObject();
			cJSON_AddNullToObject(body, "surface");
			np_host_send(&server->host, "dragIconChanged", body);
		}
		server->drag_icon = NULL;
		server->drag_focus_window = 0;
		server->drag_focus_surface = 0;
		return;
	}
	bool dropped = false;
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(surface->resource)) continue;
		struct np_data_offer *match = NULL;
		struct np_data_offer *offer;
		wl_list_for_each(offer, &server->data_offers, link) {
			if (offer->active && offer->device == device->resource &&
			    offer->source == server->drag_source) {
				match = offer;
				break;
			}
		}
		if (!match) continue;
		match->active = false;
		bool accepted = wl_resource_get_version(match->resource) < 3 ||
		                (match->accepted_mime && match->chosen_action !=
		                 WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
		if (!accepted) {
			wl_data_device_send_leave(device->resource);
			continue;
		}
		match->dropped = true;
		wl_data_device_send_drop(device->resource);
		dropped = true;
	}
	if (dropped && wl_resource_get_version(server->drag_source) >= 3) {
		wl_data_source_send_dnd_drop_performed(server->drag_source);
	}
	server->drag_dropped = dropped;
	if (!dropped) {
		struct wl_resource *source = server->drag_source;
		wl_data_source_send_cancelled(source);
		detach_offers_for_source(server, source, false);
		server->drag_source = NULL;
		server->drag_origin = NULL;
	}
	if (server->drag_icon) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNullToObject(body, "surface");
		np_host_send(&server->host, "dragIconChanged", body);
		server->drag_icon = NULL;
	}
	server->drag_focus_window = 0;
	server->drag_focus_surface = 0;
	restore_pointer_focus_after_drag(server, surface);
}

static void data_device_start_drag(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *source, struct wl_resource *origin,
                                   struct wl_resource *icon, uint32_t serial) {
	struct np_input *entry = wl_resource_get_user_data(resource);
	if (!entry || !source || !origin) return;
	struct np_server *server = entry->server;
	if (server->drag_source && server->drag_source != source) {
		struct wl_resource *previous = server->drag_source;
		wl_data_source_send_cancelled(previous);
		detach_offers_for_source(server, previous, false);
	}
	server->drag_source = source;
	server->drag_origin = origin;
	server->drag_icon = icon;
	server->drag_dropped = false;
	/* A data-device drag replaces the default pointer grab. The ordinary
	 * wl_pointer focus must leave before wl_data_device.enter takes ownership of
	 * the same physical pointer. GDK relies on this ordering: its DnD enter
	 * installs the drop focus without first clearing the normal pointer focus. */
	struct np_surface *surface = surface_by_window(server, server->pointer_window);
	if (!surface) surface = wl_resource_get_user_data(origin);
	clear_pointer_focus_for_drag(server);
	cJSON *icon_body = cJSON_CreateObject();
	if (icon) {
		struct np_surface *icon_surface = wl_resource_get_user_data(icon);
		if (icon_surface) cJSON_AddNumberToObject(icon_body, "surface", icon_surface->id);
		else cJSON_AddNullToObject(icon_body, "surface");
	} else {
		cJSON_AddNullToObject(icon_body, "surface");
	}
	np_host_send(&server->host, "dragIconChanged", icon_body);
	drag_send_enter(server, surface, fixed_from(server->pointer_x), fixed_from(server->pointer_y));
}

static void data_device_set_selection(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *source, uint32_t serial) {
	struct np_input *entry = wl_resource_get_user_data(resource);
	if (!entry) return;
	struct np_server *server = entry->server;
	server->selection_source = source;
	// A guest client taking the selection displaces the Mac's, and vice versa.
	// Keeping both would mean deciding which one a paste meant.
	for (int i = 0; i < server->host_mime_count; i++) free(server->host_mime[i]);
	server->host_mime_count = 0;
	announce_selection_to_host(server);

	struct np_surface *focused = server->focused_window
		? surface_by_window(server, server->focused_window) : NULL;
	if (!focused) return;
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(focused->resource)) continue;
		send_selection_to(server, device->resource);
	}
}

static void data_device_release(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_data_device_interface data_device_implementation = {
	.start_drag = data_device_start_drag,
	.set_selection = data_device_set_selection,
	.release = data_device_release,
};

static void data_manager_create_source(struct wl_client *client, struct wl_resource *resource,
                                       uint32_t id) {
	struct wl_resource *source = wl_resource_create(
		client, &wl_data_source_interface, wl_resource_get_version(resource), id);
	if (!source) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_data_source *state = calloc(1, sizeof(*state));
	if (!state) {
		wl_resource_destroy(source);
		wl_client_post_no_memory(client);
		return;
	}
	state->resource = source;
	wl_resource_set_implementation(source, &data_source_implementation, state,
	                               data_source_resource_destroy);
}

static void data_manager_get_device(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t id, struct wl_resource *seat) {
	struct np_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *device = wl_resource_create(
		client, &wl_data_device_interface, wl_resource_get_version(resource), id);
	if (!device) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_input *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		wl_resource_destroy(device);
		wl_client_post_no_memory(client);
		return;
	}
	entry->resource = device;
	entry->server = server;
	wl_list_insert(&server->data_devices, &entry->link);
	// The entry, not the server: input_resource_destroy unlinks whatever it is
	// given, and handing it the server would unlink a field of the server struct.
	wl_resource_set_implementation(device, &data_device_implementation, entry,
	                               input_resource_destroy);
}

static const struct wl_data_device_manager_interface data_manager_implementation = {
	.create_data_source = data_manager_create_source,
	.get_data_device = data_manager_get_device,
};

static void data_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &wl_data_device_manager_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &data_manager_implementation, data, NULL);
}

// ---------------------------------------------------------------------------
// wl_subcompositor
// ---------------------------------------------------------------------------

static void subsurface_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void subsurface_set_position(struct wl_client *client, struct wl_resource *resource,
                                    int32_t x, int32_t y) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	(void)client;
	if (!surface) return;
	surface->pending_sub_position_set = true;
	surface->pending_sub_x = x;
	surface->pending_sub_y = y;
	if (trace_enabled())
		fprintf(stderr, "[wayland] subsurface position surface=%u parent=%u %d,%d\n",
		        surface->id, surface->parent ? surface->parent->id : 0, x, y);
}

static void queue_subsurface_restack(struct wl_resource *resource,
	                                 struct wl_resource *sibling_resource,
	                                 bool above)
{
	struct np_surface *surface = wl_resource_get_user_data(resource);
	struct np_surface *sibling = wl_resource_get_user_data(sibling_resource);
	if (!surface || !surface->parent || !sibling || sibling == surface ||
	    (sibling != surface->parent && sibling->parent != surface->parent)) {
		wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
		                       "reference surface is not a sibling or parent");
		return;
	}
	struct np_subsurface_stack_op *op = calloc(1, sizeof(*op));
	if (!op) {
		wl_client_post_no_memory(wl_resource_get_client(resource));
		return;
	}
	op->child = surface;
	op->sibling = sibling;
	op->above = above;
	/* Append: requests modify pending z-order in wire order. */
	wl_list_insert(surface->parent->pending_stack_ops.prev, &op->link);
	if (trace_enabled())
		fprintf(stderr, "[wayland] subsurface place_%s surface=%u sibling=%u\n",
		        above ? "above" : "below", surface->id, sibling->id);
}

static void subsurface_place_above(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *sibling) {
	(void)client;
	queue_subsurface_restack(resource, sibling, true);
}
static void subsurface_place_below(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *sibling) {
	(void)client;
	queue_subsurface_restack(resource, sibling, false);
}

static void subsurface_set_sync(struct wl_client *client, struct wl_resource *resource) {
	((struct np_surface *)wl_resource_get_user_data(resource))->sync = true;
}
static void subsurface_set_desync(struct wl_client *client, struct wl_resource *resource) {
	(void)client;
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->sync = false;
	if (!subsurface_is_synchronized(surface)) {
		while (!wl_list_empty(&surface->synchronized_updates)) {
			struct np_surface_update *update = wl_container_of(
				surface->synchronized_updates.next, update, link);
			wl_list_remove(&update->link);
			wl_list_init(&update->link);
			apply_surface_update(update);
		}
	}
}

static const struct wl_subsurface_interface subsurface_implementation = {
	.destroy = subsurface_destroy_handler,
	.set_position = subsurface_set_position,
	.place_above = subsurface_place_above,
	.place_below = subsurface_place_below,
	.set_sync = subsurface_set_sync,
	.set_desync = subsurface_set_desync,
};

static void subsurface_resource_destroy(struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface) return;
	surface->subsurface = NULL;
	detach_from_parent(surface);
}

static void subcompositor_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void subcompositor_get_subsurface(struct wl_client *client, struct wl_resource *resource,
                                         uint32_t id, struct wl_resource *surface_resource,
                                         struct wl_resource *parent_resource) {
	struct np_surface *surface = wl_resource_get_user_data(surface_resource);
	struct np_surface *parent = wl_resource_get_user_data(parent_resource);

	surface->subsurface = wl_resource_create(
		client, &wl_subsurface_interface, wl_resource_get_version(resource), id);
	if (!surface->subsurface) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(surface->subsurface, &subsurface_implementation, surface,
	                               subsurface_resource_destroy);
	surface->parent = parent;
	surface->sync = true;  // the protocol's default
	surface->above_parent = true;
	/* A new subsurface is initially top-most among its siblings and parent. */
	wl_list_insert(parent->children.prev, &surface->sibling_link);
	if (trace_enabled())
		fprintf(stderr, "[wayland] subsurface created surface=%u parent=%u\n",
		        surface->id, parent->id);

}

static const struct wl_subcompositor_interface subcompositor_implementation = {
	.destroy = subcompositor_destroy_handler,
	.get_subsurface = subcompositor_get_subsurface,
};

static void subcompositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wl_subcompositor_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &subcompositor_implementation, data, NULL);
}

// ---------------------------------------------------------------------------
// wl_compositor
// ---------------------------------------------------------------------------

static void compositor_create_surface(struct wl_client *client, struct wl_resource *resource,
                                      uint32_t id) {
	struct np_server *server = wl_resource_get_user_data(resource);
	struct np_surface *surface = calloc(1, sizeof(*surface));
	if (!surface) {
		wl_client_post_no_memory(client);
		return;
	}
	surface->server = server;
	surface->id = server->next_id++;
	surface->scale = 1;
	surface->pending_scale = 1;
	surface->transform = WL_OUTPUT_TRANSFORM_NORMAL;
	surface->pending_transform = WL_OUTPUT_TRANSFORM_NORMAL;
	wl_list_init(&surface->children);
	wl_list_init(&surface->sibling_link);
	wl_list_init(&surface->pending_stack_ops);
	wl_list_init(&surface->pending_frame_callbacks);
	wl_list_init(&surface->blocked_updates);
	wl_list_init(&surface->scene_presentations);
	wl_list_init(&surface->synchronized_updates);
	wl_list_init(&surface->current_buffer_destroy.link);
	surface->resource = wl_resource_create(
		client, &wl_surface_interface, wl_resource_get_version(resource), id);
	if (!surface->resource) {
		free(surface);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(surface->resource, &surface_implementation, surface,
	                               surface_resource_destroy);
	wl_list_insert(&server->surfaces, &surface->link);

	// A surface learns integer buffer scale from the outputs it has entered.
	// Advertising wl_output.scale without enter leaves GTK/Qt with no applicable
	// output and therefore no well-defined surface scale.
	np_scale_surface_enter_outputs(surface, client);
	np_host_send(&server->host, "surfaceCreated", object_with_u32("surface", surface->id));
}

static void compositor_create_region(struct wl_client *client, struct wl_resource *resource,
                                     uint32_t id) {
	(void)resource;
	np_region_create(client, id);
}

static const struct wl_compositor_interface compositor_implementation = {
	.create_surface = compositor_create_surface,
	.create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wl_compositor_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &compositor_implementation, data, NULL);
}

// ---------------------------------------------------------------------------
// xdg_shell
// ---------------------------------------------------------------------------

static void toplevel_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void toplevel_set_parent(struct wl_client *client, struct wl_resource *resource,
                                struct wl_resource *parent) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	if (parent) {
		struct np_surface *other = wl_resource_get_user_data(parent);
		cJSON_AddNumberToObject(body, "parent", other->window_id);
	} else {
		cJSON_AddNullToObject(body, "parent");
	}
	np_host_send(&surface->server->host, "parentChanged", body);
}

static void toplevel_set_title(struct wl_client *client, struct wl_resource *resource,
                               const char *title) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	cJSON_AddStringToObject(body, "title", title ? title : "");
	free(surface->title);
	surface->title = strdup(title ? title : "");
	np_host_send(&surface->server->host, "titleChanged", body);
}

static void toplevel_set_app_id(struct wl_client *client, struct wl_resource *resource,
                                const char *app_id) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	cJSON_AddStringToObject(body, "appID", app_id ? app_id : "");
	free(surface->app_id);
	surface->app_id = strdup(app_id ? app_id : "");
	np_host_send(&surface->server->host, "appIDChanged", body);
}

static void toplevel_show_window_menu(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *seat, uint32_t serial,
                                      int32_t x, int32_t y) {}

static void toplevel_move(struct wl_client *client, struct wl_resource *resource,
                          struct wl_resource *seat, uint32_t serial) {
	// Client-side decorations report title-bar drags here, which is why the host
	// never has to infer a draggable region.
	struct np_surface *surface = wl_resource_get_user_data(resource);
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	cJSON_AddNumberToObject(body, "serial", serial);
	np_host_send(&surface->server->host, "interactiveMoveRequested", body);
}

static void toplevel_resize(struct wl_client *client, struct wl_resource *resource,
                            struct wl_resource *seat, uint32_t serial, uint32_t edges) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	cJSON_AddNumberToObject(body, "edges", edges);
	cJSON_AddNumberToObject(body, "serial", serial);
	np_host_send(&surface->server->host, "interactiveResizeRequested", body);
}

static void add_size_constraint(cJSON *body, const char *key,
                                int32_t width, int32_t height) {
	if (width > 0 && height > 0) {
		cJSON *size = cJSON_CreateObject();
		cJSON_AddNumberToObject(size, "width", width);
		cJSON_AddNumberToObject(size, "height", height);
		cJSON_AddItemToObject(body, key, size);
	} else {
		cJSON_AddNullToObject(body, key);
	}
}

static void send_size_constraints(struct np_surface *surface) {
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	/* Always publish a complete snapshot.  Optional associated values cannot
	 * distinguish an omitted key from an explicit protocol reset after JSON
	 * decoding, and a partial update was also lost when NSWindow did not exist
	 * yet. */
	add_size_constraint(body, "minimum", surface->minimum_width,
	                    surface->minimum_height);
	add_size_constraint(body, "maximum", surface->maximum_width,
	                    surface->maximum_height);
	np_host_send(&surface->server->host, "sizeConstraintsChanged", body);
}

static void toplevel_set_max_size(struct wl_client *client, struct wl_resource *resource,
                                  int32_t width, int32_t height) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->maximum_width = width;
	surface->maximum_height = height;
	send_size_constraints(surface);
}

static void toplevel_set_min_size(struct wl_client *client, struct wl_resource *resource,
                                  int32_t width, int32_t height) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->minimum_width = width;
	surface->minimum_height = height;
	send_size_constraints(surface);
}

static void toplevel_request_flag(struct np_surface *surface, const char *name, bool enabled) {
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	cJSON_AddBoolToObject(body, "enabled", enabled);
	np_host_send(&surface->server->host, name, body);
}

static void toplevel_set_maximized(struct wl_client *client, struct wl_resource *resource) {
	toplevel_request_flag(wl_resource_get_user_data(resource), "maximizeRequested", true);
}
static void toplevel_unset_maximized(struct wl_client *client, struct wl_resource *resource) {
	toplevel_request_flag(wl_resource_get_user_data(resource), "maximizeRequested", false);
}
static void toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *resource,
                                    struct wl_resource *output) {
	toplevel_request_flag(wl_resource_get_user_data(resource), "fullscreenRequested", true);
}
static void toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *resource) {
	toplevel_request_flag(wl_resource_get_user_data(resource), "fullscreenRequested", false);
}
static void toplevel_set_minimized(struct wl_client *client, struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	np_host_send(&surface->server->host, "minimizeRequested",
	             object_with_u32("window", surface->window_id));
}

static const struct xdg_toplevel_interface toplevel_implementation = {
	.destroy = toplevel_destroy_handler,
	.set_parent = toplevel_set_parent,
	.set_title = toplevel_set_title,
	.set_app_id = toplevel_set_app_id,
	.show_window_menu = toplevel_show_window_menu,
	.move = toplevel_move,
	.resize = toplevel_resize,
	.set_max_size = toplevel_set_max_size,
	.set_min_size = toplevel_set_min_size,
	.set_maximized = toplevel_set_maximized,
	.unset_maximized = toplevel_unset_maximized,
	.set_fullscreen = toplevel_set_fullscreen,
	.unset_fullscreen = toplevel_unset_fullscreen,
	.set_minimized = toplevel_set_minimized,
};

static void toplevel_resource_destroy(struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface) return;
	np_host_send(&surface->server->host, "toplevelDestroyed",
	             object_with_u32("window", surface->window_id));
	surface->toplevel = NULL;
}

enum np_configure_state_bits {
	NP_CONFIGURE_MAXIMIZED = 1u << 0,
	NP_CONFIGURE_FULLSCREEN = 1u << 1,
	NP_CONFIGURE_RESIZING = 1u << 2,
	NP_CONFIGURE_ACTIVATED = 1u << 3,
};

static void append_toplevel_state(struct wl_array *states, uint32_t value) {
	uint32_t *slot = wl_array_add(states, sizeof(*slot));
	if (slot) *slot = value;
}

static void send_host_toplevel_configure(struct np_surface *surface,
	                                     int32_t width, int32_t height,
	                                     uint32_t state_bits) {
	if (!surface || !surface->toplevel || !surface->xdg_surface) return;
	struct wl_array states;
	wl_array_init(&states);
	if (state_bits & NP_CONFIGURE_MAXIMIZED)
		append_toplevel_state(&states, XDG_TOPLEVEL_STATE_MAXIMIZED);
	if (state_bits & NP_CONFIGURE_FULLSCREEN)
		append_toplevel_state(&states, XDG_TOPLEVEL_STATE_FULLSCREEN);
	if (state_bits & NP_CONFIGURE_RESIZING)
		append_toplevel_state(&states, XDG_TOPLEVEL_STATE_RESIZING);
	if (state_bits & NP_CONFIGURE_ACTIVATED)
		append_toplevel_state(&states, XDG_TOPLEVEL_STATE_ACTIVATED);

	// Host sizes arrive in AppKit points. A Wayland configure is also expressed
	// in logical window-geometry coordinates; buffer_scale only controls how many
	// pixels the client attaches and must not be applied to this size.
	xdg_toplevel_send_configure(surface->toplevel, width, height, &states);
	wl_array_release(&states);
	uint32_t serial = ++surface->configure_serial;
	if (!serial) serial = ++surface->configure_serial;
	xdg_surface_send_configure(surface->xdg_surface, serial);
	if (trace_enabled())
		fprintf(stderr,
		        "[configure] send surface=%u serial=%u size=%dx%d states=0x%x\n",
		        surface->id, serial, width, height, state_bits);
	surface->host_configure_in_flight = true;
	surface->host_configure_serial = serial;
}

static void queue_host_toplevel_configure(struct np_surface *surface,
	                                      int32_t width, int32_t height,
	                                      uint32_t state_bits) {
	/* Keep at most one configure awaiting a client ack+commit. An already
	 * scheduled idle is also a coalescing interval: overwrite its value instead
	 * of immediately sending the first stale host event in the queue. */
	if (surface->host_configure_in_flight || surface->host_configure_idle) {
		surface->host_configure_pending = true;
		surface->host_configure_pending_width = width;
		surface->host_configure_pending_height = height;
		surface->host_configure_pending_state_bits = state_bits;
		return;
	}
	send_host_toplevel_configure(surface, width, height, state_bits);
}

static void send_pending_host_toplevel_configure(struct np_surface *surface) {
	if (!surface || surface->host_configure_in_flight ||
	    !surface->host_configure_pending)
		return;
	int32_t width = surface->host_configure_pending_width;
	int32_t height = surface->host_configure_pending_height;
	uint32_t state_bits = surface->host_configure_pending_state_bits;
	surface->host_configure_pending = false;
	send_host_toplevel_configure(surface, width, height, state_bits);
}

static void dispatch_pending_host_toplevel_configure(void *data) {
	struct np_surface *surface = data;
	surface->host_configure_idle = NULL;
	send_pending_host_toplevel_configure(surface);
}

static void schedule_pending_host_toplevel_configure(struct np_surface *surface) {
	if (!surface || surface->host_configure_idle ||
	    !surface->host_configure_pending)
		return;
	struct wl_event_loop *loop = wl_display_get_event_loop(surface->server->display);
	surface->host_configure_idle = wl_event_loop_add_idle(
		loop, dispatch_pending_host_toplevel_configure, surface);
}

static void finish_host_toplevel_configure(struct np_surface *surface,
	                                       uint32_t serial) {
	if (!surface || !serial) return;
	if (trace_enabled())
		fprintf(stderr,
		        "[configure] ready surface=%u acked=%u current=%u serial=%u pending=%d\n",
		        surface->id, surface->host_configure_acked_serial,
		        surface->host_configure_serial, serial,
		        surface->host_configure_pending);
	if (serial == surface->host_configure_serial) {
		surface->host_configure_in_flight = false;
		schedule_pending_host_toplevel_configure(surface);
	}
	if (surface->host_configure_acked_serial == serial) {
		surface->host_configure_acked_serial = 0;
		surface->host_configure_acked = false;
	}
}

static void xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *resource,
                                     uint32_t id) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->toplevel = wl_resource_create(
		client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
	if (!surface->toplevel) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(surface->toplevel, &toplevel_implementation, surface,
	                               toplevel_resource_destroy);
	surface->window_id = surface->server->next_id++;

	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	cJSON_AddNumberToObject(body, "surface", surface->id);
	np_host_send(&surface->server->host, "toplevelCreated", body);
	/* Decoration policy is compositor state.  Send the client-side default
	 * before the first frame so AppKit never constructs a transient second
	 * titlebar around a CSD window. */
	np_decoration_use_client_default(surface);

	// Give the client a concrete initial size. Sending 0x0 means "you choose",
	// but the host also waits for the first frame before creating NSWindow; that
	// leaves some clients trying to allocate a zero-sized Cairo shm surface.
	// The client may still clamp or replace this suggestion before committing.
	send_host_toplevel_configure(surface, 800, 600, 0);
}

/// Where the client wants a popup, expressed relative to a rectangle on its
/// parent. Resolving it is the compositor's job, and getting it wrong puts menus
/// in the wrong place rather than failing outright.
struct np_positioner {
	int32_t width, height;
	int32_t anchor_x, anchor_y, anchor_width, anchor_height;
	uint32_t anchor;
	uint32_t gravity;
	int32_t offset_x, offset_y;
};

static void positioner_resolve(const struct np_positioner *p, int32_t *out_x, int32_t *out_y) {
	// The anchor picks a point on the rectangle...
	int32_t x = p->anchor_x, y = p->anchor_y;
	switch (p->anchor) {
	case XDG_POSITIONER_ANCHOR_TOP:          x += p->anchor_width / 2; break;
	case XDG_POSITIONER_ANCHOR_BOTTOM:       x += p->anchor_width / 2; y += p->anchor_height; break;
	case XDG_POSITIONER_ANCHOR_LEFT:         y += p->anchor_height / 2; break;
	case XDG_POSITIONER_ANCHOR_RIGHT:        x += p->anchor_width; y += p->anchor_height / 2; break;
	case XDG_POSITIONER_ANCHOR_TOP_LEFT:     break;
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:  y += p->anchor_height; break;
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT:    x += p->anchor_width; break;
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: x += p->anchor_width; y += p->anchor_height; break;
	default:                                 x += p->anchor_width / 2; y += p->anchor_height / 2; break;
	}

	// ...and the gravity says which way the popup grows from it.
	switch (p->gravity) {
	case XDG_POSITIONER_GRAVITY_TOP:          x -= p->width / 2; y -= p->height; break;
	case XDG_POSITIONER_GRAVITY_BOTTOM:       x -= p->width / 2; break;
	case XDG_POSITIONER_GRAVITY_LEFT:         x -= p->width; y -= p->height / 2; break;
	case XDG_POSITIONER_GRAVITY_RIGHT:        y -= p->height / 2; break;
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:     x -= p->width; y -= p->height; break;
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:  x -= p->width; break;
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:    y -= p->height; break;
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: break;
	default:                                  x -= p->width / 2; y -= p->height / 2; break;
	}

	*out_x = x + p->offset_x;
	*out_y = y + p->offset_y;
}

static void positioner_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void positioner_set_size(struct wl_client *client, struct wl_resource *resource,
                                int32_t width, int32_t height) {
	struct np_positioner *p = wl_resource_get_user_data(resource);
	p->width = width; p->height = height;
}
static void positioner_set_anchor_rect(struct wl_client *client, struct wl_resource *resource,
                                       int32_t x, int32_t y, int32_t width, int32_t height) {
	struct np_positioner *p = wl_resource_get_user_data(resource);
	p->anchor_x = x; p->anchor_y = y; p->anchor_width = width; p->anchor_height = height;
}
static void positioner_set_anchor(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t anchor) {
	((struct np_positioner *)wl_resource_get_user_data(resource))->anchor = anchor;
}
static void positioner_set_gravity(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t gravity) {
	((struct np_positioner *)wl_resource_get_user_data(resource))->gravity = gravity;
}
static void positioner_set_constraint_adjustment(struct wl_client *client,
                                                 struct wl_resource *resource,
                                                 uint32_t adjustment) {
	// Keeping a popup on screen is macOS's problem: it knows where the screens
	// are and the guest does not.
}
static void positioner_set_offset(struct wl_client *client, struct wl_resource *resource,
                                  int32_t x, int32_t y) {
	struct np_positioner *p = wl_resource_get_user_data(resource);
	p->offset_x = x; p->offset_y = y;
}
static void positioner_set_reactive(struct wl_client *client, struct wl_resource *resource) {}
static void positioner_set_parent_size(struct wl_client *client, struct wl_resource *resource,
                                       int32_t width, int32_t height) {}
static void positioner_set_parent_configure(struct wl_client *client, struct wl_resource *resource,
                                            uint32_t serial) {}

static const struct xdg_positioner_interface positioner_implementation = {
	.destroy = positioner_destroy_handler,
	.set_size = positioner_set_size,
	.set_anchor_rect = positioner_set_anchor_rect,
	.set_anchor = positioner_set_anchor,
	.set_gravity = positioner_set_gravity,
	.set_constraint_adjustment = positioner_set_constraint_adjustment,
	.set_offset = positioner_set_offset,
	.set_reactive = positioner_set_reactive,
	.set_parent_size = positioner_set_parent_size,
	.set_parent_configure = positioner_set_parent_configure,
};

static void positioner_resource_destroy(struct wl_resource *resource) {
	free(wl_resource_get_user_data(resource));
}

// ---- popup ----

static void popup_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void popup_grab(struct wl_client *client, struct wl_resource *resource,
                       struct wl_resource *seat, uint32_t serial) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface) return;
	struct np_server *server = surface->server;

	// The grab is what makes a menu dismiss when the user clicks elsewhere. The
	// host decides when that has happened and sends dismissPopup back.
	//
	// Keyboard focus, though, has to move here rather than follow the host. A
	// popup is a non-activating NSPanel, so it never becomes key and macOS goes
	// on reporting the parent as focused. Leaving focus there would send the
	// menu's arrow keys and mnemonics to the surface underneath it.
	surface->has_grab = true;
	surface->focus_restore_window = server->focused_window;
	set_keyboard_focus(server, surface->window_id);
	wl_display_flush_clients(server->display);
}

static void popup_reposition(struct wl_client *client, struct wl_resource *resource,
                             struct wl_resource *positioner, uint32_t token) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	struct np_positioner *p = wl_resource_get_user_data(positioner);
	int32_t x = 0, y = 0;
	positioner_resolve(p, &x, &y);
	if (wl_resource_get_version(resource) >= XDG_POPUP_REPOSITIONED_SINCE_VERSION) {
		xdg_popup_send_repositioned(resource, token);
	}
	xdg_popup_send_configure(resource, x, y, p->width, p->height);
	xdg_surface_send_configure(surface->xdg_surface, ++surface->configure_serial);
}

static const struct xdg_popup_interface popup_implementation = {
	.destroy = popup_destroy_handler,
	.grab = popup_grab,
	.reposition = popup_reposition,
};

static void popup_resource_destroy(struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface) return;
	if (surface->has_grab) {
		struct np_server *server = surface->server;
		surface->has_grab = false;
		// Only hand focus back if this popup still holds it; an inner level may
		// already have moved on.
		if (server->focused_window == surface->window_id) {
			uint32_t restore = surface->focus_restore_window;
			set_keyboard_focus(server,
			                   surface_by_window(server, restore) ? restore : 0);
		}
	}
	np_host_send(&surface->server->host, "popupDestroyed",
	             object_with_u32("window", surface->window_id));
	surface->popup = NULL;
}

static void xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id, struct wl_resource *parent,
                                  struct wl_resource *positioner) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	struct np_surface *parent_surface = parent ? wl_resource_get_user_data(parent) : NULL;
	struct np_positioner *p = wl_resource_get_user_data(positioner);

	surface->popup = wl_resource_create(
		client, &xdg_popup_interface, wl_resource_get_version(resource), id);
	if (!surface->popup) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(surface->popup, &popup_implementation, surface,
	                               popup_resource_destroy);
	surface->window_id = surface->server->next_id++;

	int32_t x = 0, y = 0;
	positioner_resolve(p, &x, &y);

	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	cJSON_AddNumberToObject(body, "surface", surface->id);
	cJSON_AddNumberToObject(body, "parent", parent_surface ? parent_surface->window_id : 0);
	cJSON_AddNumberToObject(body, "x", x);
	cJSON_AddNumberToObject(body, "y", y);
	cJSON_AddNumberToObject(body, "width", p->width);
	cJSON_AddNumberToObject(body, "height", p->height);
	np_host_send(&surface->server->host, "popupCreated", body);

	xdg_popup_send_configure(surface->popup, x, y, p->width, p->height);
	xdg_surface_send_configure(surface->xdg_surface, ++surface->configure_serial);
}

static void xdg_surface_set_window_geometry(struct wl_client *client, struct wl_resource *resource,
	                                        int32_t x, int32_t y, int32_t width, int32_t height) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE,
		                       "window geometry must have positive dimensions");
		return;
	}
	surface->pending_geometry_set = true;
	surface->pending_geometry_x = x;
	surface->pending_geometry_y = y;
	surface->pending_geometry_width = width;
	surface->pending_geometry_height = height;
	if (trace_enabled())
		fprintf(stderr, "[wayland] window geometry surface=%u %d,%d %dx%d\n",
		        surface->id, x, y, width, height);
}

static void xdg_surface_ack_configure(struct wl_client *client, struct wl_resource *resource,
	                                  uint32_t serial) {
	(void)client;
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface || !surface->host_configure_in_flight || !serial ||
	    serial > surface->host_configure_serial)
		return;
	// Acking a serial discards every older configure. Accept the newest ack seen
	// so far, but never an impossible serial newer than the compositor emitted.
	if (surface->host_configure_acked_serial &&
	    serial < surface->host_configure_acked_serial)
		return;
	surface->host_configure_acked_serial = serial;
	surface->host_configure_acked = true;
	if (trace_enabled())
		fprintf(stderr,
		        "[configure] ack surface=%u serial=%u current=%u pending=%d\n",
		        surface->id, serial, surface->host_configure_serial,
		        surface->host_configure_pending);
}

static void xdg_surface_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void xdg_surface_resource_destroy(struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (surface && surface->xdg_surface == resource)
		surface->xdg_surface = NULL;
}

static const struct xdg_surface_interface xdg_surface_implementation = {
	.destroy = xdg_surface_destroy_handler,
	.get_toplevel = xdg_surface_get_toplevel,
	.get_popup = xdg_surface_get_popup,
	.set_window_geometry = xdg_surface_set_window_geometry,
	.ack_configure = xdg_surface_ack_configure,
};

static void wm_base_get_xdg_surface(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t id, struct wl_resource *surface_resource) {
	struct np_surface *surface = wl_resource_get_user_data(surface_resource);
	surface->xdg_surface = wl_resource_create(
		client, &xdg_surface_interface, wl_resource_get_version(resource), id);
	if (!surface->xdg_surface) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(surface->xdg_surface, &xdg_surface_implementation,
	                               surface, xdg_surface_resource_destroy);
}

static void wm_base_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void wm_base_create_positioner(struct wl_client *client, struct wl_resource *resource,
                                      uint32_t id) {
	struct wl_resource *positioner = wl_resource_create(
		client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
	if (!positioner) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_positioner *state = calloc(1, sizeof(*state));
	if (!state) {
		wl_resource_destroy(positioner);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(positioner, &positioner_implementation, state,
	                               positioner_resource_destroy);
}
static void wm_base_pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {}

static const struct xdg_wm_base_interface wm_base_implementation = {
	.destroy = wm_base_destroy_handler,
	.create_positioner = wm_base_create_positioner,
	.get_xdg_surface = wm_base_get_xdg_surface,
	.pong = wm_base_pong,
};

static void wm_base_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &xdg_wm_base_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &wm_base_implementation, data, NULL);
}

// ---------------------------------------------------------------------------
// wl_seat and wl_output
// ---------------------------------------------------------------------------

#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

/// Compiles the keymap clients will use to turn evdev codes into characters.
///
/// The host sends physical key codes and nothing else — no text, no composed
/// characters — so *something* has to define the layout, and xkbcommon is what
/// every Wayland client already links against to read it.
static bool create_keymap(struct np_server *server) {
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) return false;

	struct xkb_rule_names names = {
		.rules = NULL, .model = "pc105", .layout = "us",
		.variant = NULL, .options = NULL,
	};
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names,
	                                                      XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		xkb_context_unref(context);
		return false;
	}

	char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	if (!text) return false;

	size_t size = strlen(text) + 1;
	int fd = memfd_create("nativepipe-keymap", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		free(text);
		if (fd >= 0) close(fd);
		return false;
	}
	void *mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED) {
		free(text);
		close(fd);
		return false;
	}
	memcpy(mapped, text, size);
	munmap(mapped, size);
	free(text);

	server->keymap_fd = fd;
	server->keymap_size = size;
	return true;
}

static void input_resource_destroy(struct wl_resource *resource) {
	struct np_input *input = wl_resource_get_user_data(resource);
	if (!input) return;
	/* np_input is shared by pointer, keyboard and data-device bindings. Only a
	 * data device can be referenced by an offer; clear that non-owning pointer
	 * before libwayland may recycle the wl_resource allocation. */
	struct np_data_offer *offer;
	wl_list_for_each(offer, &input->server->data_offers, link) {
		if (offer->device != resource) continue;
		offer->device = NULL;
		offer->active = false;
	}
	wl_list_remove(&input->link);
	free(input);
}

/// The pointer image belongs to the macOS window system. Clients still issue
/// wl_pointer.set_cursor after enter, and the request must be consumed even
/// when NativePipe does not use the supplied cursor surface. Registering a NULL
/// implementation makes libwayland abort the client connection on that first
/// perfectly valid request.
static void pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
                               uint32_t serial, struct wl_resource *surface,
                               int32_t hotspot_x, int32_t hotspot_y) {}

static void pointer_release(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_implementation = {
	.set_cursor = pointer_set_cursor,
	.release = pointer_release,
};

static void keyboard_release(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_implementation = {
	.release = keyboard_release,
};

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct np_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *pointer =
		wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
	if (!pointer) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_input *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		wl_resource_destroy(pointer);
		wl_client_post_no_memory(client);
		return;
	}
	entry->resource = pointer;
	entry->server = server;
	wl_list_insert(&server->pointers, &entry->link);
	wl_resource_set_implementation(
		pointer, &pointer_implementation, entry, input_resource_destroy);
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct np_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *keyboard =
		wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
	if (!keyboard) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_input *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		wl_resource_destroy(keyboard);
		wl_client_post_no_memory(client);
		return;
	}
	entry->resource = keyboard;
	entry->server = server;
	wl_list_insert(&server->keyboards, &entry->link);
	wl_resource_set_implementation(
		keyboard, &keyboard_implementation, entry, input_resource_destroy);

	if (server->keymap_fd >= 0) {
		wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
		                        server->keymap_fd, (uint32_t)server->keymap_size);
	}
	if (wl_resource_get_version(keyboard) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
		// Wayland clients own key repeat. The host filters AppKit repeat events,
		// leaving one physical press/release pair for this policy to act on.
		wl_keyboard_send_repeat_info(keyboard, 25, 600);
	}
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id) {}
static void seat_release(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_implementation = {
	.get_pointer = seat_get_pointer,
	.get_keyboard = seat_get_keyboard,
	.get_touch = seat_get_touch,
	.release = seat_release,
};

/// Advertised so clients bind and proceed; events are delivered only to the
/// client owning the surface the host says is focused or under the pointer.
static void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wl_seat_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &seat_implementation, data, NULL);
	wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
	if (version >= WL_SEAT_NAME_SINCE_VERSION) wl_seat_send_name(resource, "NativePipe");
}

static void pointer_frame(struct wl_resource *pointer) {
	if (wl_resource_get_version(pointer) >= WL_POINTER_FRAME_SINCE_VERSION) {
		wl_pointer_send_frame(pointer);
	}
}

/// A data-device drag replaces the default pointer grab. Clear ordinary
/// wl_pointer focus before sending wl_data_device.enter, matching the ordering
/// used by full compositors. Without this leave, GDK carries widget hover state
/// into the DnD grab and the first post-drag click merely repairs that state.
static void clear_pointer_focus_for_drag(struct np_server *server) {
	if (!server || !server->pointer_window) return;
	struct np_surface *surface = surface_by_id(server, server->pointer_surface);
	if (!surface) surface = surface_by_window(server, server->pointer_window);
	if (surface) {
		uint32_t serial = wl_display_next_serial(server->display);
		struct np_input *entry;
		wl_list_for_each(entry, &server->pointers, link) {
			if (!same_client(entry->resource, surface->resource)) continue;
			wl_pointer_send_leave(entry->resource, serial, surface->resource);
			pointer_frame(entry->resource);
		}
	}
	server->pointer_window = 0;
	server->pointer_surface = 0;
}

/// Ending the DnD grab and finishing its data transfer are separate moments.
/// Re-establish ordinary pointer focus immediately at button release; waiting
/// for wl_data_offer.finish makes the window ignore input while the target is
/// still reading the dragged bytes.
static void restore_pointer_focus_after_drag(struct np_server *server,
	                                         struct np_surface *surface) {
	if (!server || !surface) return;
	double local_x = server->pointer_x, local_y = server->pointer_y;
	struct np_surface *root = np_scene_root(surface);
	struct np_surface *target = np_scene_hit_test(
		root ? root : surface, server->pointer_x, server->pointer_y,
		&local_x, &local_y);
	if (!target) target = surface;
	uint32_t serial = wl_display_next_serial(server->display);
	struct np_input *entry;

	/* clear_pointer_focus_for_drag() already sent the leave at grab start. At
	 * grab end the default pointer focus is empty, so only enter is valid here. */
	wl_list_for_each(entry, &server->pointers, link) {
		if (!same_client(entry->resource, target->resource)) continue;
		wl_pointer_send_enter(entry->resource, serial, target->resource,
		                      fixed_from(local_x), fixed_from(local_y));
		pointer_frame(entry->resource);
	}
	server->pointer_window = surface->window_id;
	server->pointer_surface = target->id;
}

// ---------------------------------------------------------------------------
// host commands
// ---------------------------------------------------------------------------

static int json_int(cJSON *object, const char *key, int fallback) {
	cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
	return cJSON_IsNumber(item) ? (int)item->valuedouble : fallback;
}

static double json_double(cJSON *object, const char *key, double fallback) {
	cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
	return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool same_client(struct wl_resource *a, struct wl_resource *b) {
	return wl_resource_get_client(a) == wl_resource_get_client(b);
}

/// Delivers keyboard focus, which in Wayland is leave-then-enter rather than a
/// single "focus is now X" event.

// ---------------------------------------------------------------------------
// zwp_text_input_v3
//
// This is the IME seam, and it is a narrow one on purpose. The client says "a
// text field is focused, and the caret is here"; macOS composes; the client is
// handed finished text and a preedit string. Nothing in between crosses the
// boundary — no candidate list, no conversion state, no key interception —
// because all of that is the input method's business and macOS already has one.
//
// Keys that are not text keep going down the raw evdev path. Return, Escape and
// the arrow keys are editing commands rather than characters, and a client that
// stopped receiving them while a text field was focused would be unusable.
// ---------------------------------------------------------------------------

struct np_text_input {
	struct wl_list link;
	struct wl_resource *resource;
	struct np_server *server;
	/// Applied state, and the pending half that `commit` promotes.
	bool enabled;
	bool pending_enabled;
	bool pending_cursor_set;
	int32_t pending_cursor_x, pending_cursor_y;
	int32_t pending_cursor_width, pending_cursor_height;
	char *pending_surrounding;
	int32_t pending_cursor_index, pending_anchor_index;
	/// Echoed back in `done`. The client uses it to discard events that
	/// describe a state it has already moved on from.
	uint32_t serial;
};

static void text_input_enable(struct wl_client *client, struct wl_resource *resource) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (input) input->pending_enabled = true;
}

static void text_input_disable(struct wl_client *client, struct wl_resource *resource) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (input) input->pending_enabled = false;
}

static void text_input_set_surrounding_text(struct wl_client *client,
                                            struct wl_resource *resource,
                                            const char *text, int32_t cursor,
                                            int32_t anchor) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input) return;
	free(input->pending_surrounding);
	input->pending_surrounding = strdup(text ? text : "");
	input->pending_cursor_index = cursor;
	input->pending_anchor_index = anchor;
}

static void text_input_set_text_change_cause(struct wl_client *client,
                                             struct wl_resource *resource, uint32_t cause) {
	// Only distinguishes IME-caused edits from other ones, which matters to an
	// input method running inside the compositor. Ours runs on the Mac.
}

static void text_input_set_content_type(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t hint, uint32_t purpose) {
	// Password and digit fields would map onto NSTextInputContext hints; v1
	// leaves the Mac's default behaviour alone rather than guessing at it.
}

static void text_input_set_cursor_rectangle(struct wl_client *client,
                                            struct wl_resource *resource,
                                            int32_t x, int32_t y, int32_t width, int32_t height) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input) return;
	input->pending_cursor_set = true;
	input->pending_cursor_x = x;
	input->pending_cursor_y = y;
	input->pending_cursor_width = width;
	input->pending_cursor_height = height;
}

static void text_input_commit(struct wl_client *client, struct wl_resource *resource) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input) return;
	struct np_server *server = input->server;
	input->serial++;

	bool was_enabled = input->enabled;
	input->enabled = input->pending_enabled;

	if (was_enabled != input->enabled) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "window", server->focused_window);
		cJSON_AddBoolToObject(body, "enabled", input->enabled);
		np_host_send(&server->host, "textInputEnabled", body);
	}
	if (input->enabled && input->pending_cursor_set) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "window", server->focused_window);
		cJSON_AddNumberToObject(body, "x", input->pending_cursor_x);
		cJSON_AddNumberToObject(body, "y", input->pending_cursor_y);
		cJSON_AddNumberToObject(body, "width", input->pending_cursor_width);
		cJSON_AddNumberToObject(body, "height", input->pending_cursor_height);
		np_host_send(&server->host, "textInputCursorRect", body);
		input->pending_cursor_set = false;
	}
	if (input->enabled && input->pending_surrounding) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "window", server->focused_window);
		cJSON_AddStringToObject(body, "text", input->pending_surrounding);
		cJSON_AddNumberToObject(body, "cursor", input->pending_cursor_index);
		cJSON_AddNumberToObject(body, "anchor", input->pending_anchor_index);
		np_host_send(&server->host, "textInputSurroundingText", body);
		free(input->pending_surrounding);
		input->pending_surrounding = NULL;
	}
}

static void text_input_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_text_input_v3_interface text_input_implementation = {
	.destroy = text_input_destroy_handler,
	.enable = text_input_enable,
	.disable = text_input_disable,
	.set_surrounding_text = text_input_set_surrounding_text,
	.set_text_change_cause = text_input_set_text_change_cause,
	.set_content_type = text_input_set_content_type,
	.set_cursor_rectangle = text_input_set_cursor_rectangle,
	.commit = text_input_commit,
};

static void text_input_resource_destroy(struct wl_resource *resource) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input) return;
	if (input->enabled) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "window", input->server->focused_window);
		cJSON_AddBoolToObject(body, "enabled", false);
		np_host_send(&input->server->host, "textInputEnabled", body);
	}
	wl_list_remove(&input->link);
	free(input->pending_surrounding);
	free(input);
}

/// Tells each client's text input whether the focused surface is now theirs.
/// v3 requires enter/leave to track keyboard focus exactly.
static void text_input_focus_changed(struct np_server *server,
                                     struct np_surface *previous,
                                     struct np_surface *next) {
	struct np_text_input *input;
	wl_list_for_each(input, &server->text_inputs, link) {
		struct wl_client *owner = wl_resource_get_client(input->resource);
		if (previous && owner == wl_resource_get_client(previous->resource)) {
			zwp_text_input_v3_send_leave(input->resource, previous->resource);
			// Leaving disables the field, per the protocol. Saying so keeps the
			// Mac's input context from staying armed for a window that is gone.
			if (input->enabled) {
				input->enabled = false;
				input->pending_enabled = false;
				cJSON *body = cJSON_CreateObject();
				cJSON_AddNumberToObject(body, "window",
				                        previous ? previous->window_id : 0);
				cJSON_AddBoolToObject(body, "enabled", false);
				np_host_send(&server->host, "textInputEnabled", body);
			}
		}
		if (next && owner == wl_resource_get_client(next->resource)) {
			zwp_text_input_v3_send_enter(input->resource, next->resource);
		}
	}
}

/// Delivers what the macOS input method produced. Everything in one batch is
/// applied by the trailing `done`, which is what makes a preedit replacement
/// atomic instead of a flicker through the empty string.
static void text_input_deliver(struct np_server *server, const char *commit_text,
                               const char *preedit_text, int32_t cursor_begin,
                               int32_t cursor_end, int32_t delete_before,
                               int32_t delete_after) {
	struct np_surface *focused = server->focused_window
		? surface_by_window(server, server->focused_window) : NULL;
	if (!focused) return;

	struct np_text_input *input;
	wl_list_for_each(input, &server->text_inputs, link) {
		if (!input->enabled) continue;
		if (wl_resource_get_client(input->resource) !=
		    wl_resource_get_client(focused->resource)) continue;
		if (delete_before || delete_after) {
			zwp_text_input_v3_send_delete_surrounding_text(
				input->resource, (uint32_t)delete_before, (uint32_t)delete_after);
		}
		if (preedit_text) {
			zwp_text_input_v3_send_preedit_string(
				input->resource, preedit_text[0] ? preedit_text : NULL,
				cursor_begin, cursor_end);
		}
		if (commit_text && commit_text[0]) {
			zwp_text_input_v3_send_commit_string(input->resource, commit_text);
		}
		zwp_text_input_v3_send_done(input->resource, input->serial);
	}
	wl_display_flush_clients(server->display);
}

static void text_input_manager_get(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t id, struct wl_resource *seat) {
	struct np_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *created = wl_resource_create(
		client, &zwp_text_input_v3_interface, wl_resource_get_version(resource), id);
	if (!created) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_text_input *input = calloc(1, sizeof(*input));
	if (!input) {
		wl_resource_destroy(created);
		wl_client_post_no_memory(client);
		return;
	}
	input->resource = created;
	input->server = server;
	wl_list_insert(&server->text_inputs, &input->link);
	wl_resource_set_implementation(created, &text_input_implementation, input,
	                               text_input_resource_destroy);

	// A client that binds while already focused must be told so, or it will sit
	// waiting for an enter that already happened.
	struct np_surface *focused = server->focused_window
		? surface_by_window(server, server->focused_window) : NULL;
	if (focused && wl_resource_get_client(focused->resource) == client) {
		zwp_text_input_v3_send_enter(created, focused->resource);
	}
}

static void text_input_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_text_input_manager_v3_interface text_input_manager_implementation = {
	.destroy = text_input_manager_destroy,
	.get_text_input = text_input_manager_get,
};

static void text_input_manager_bind(struct wl_client *client, void *data,
                                    uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &zwp_text_input_manager_v3_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &text_input_manager_implementation, data, NULL);
}

/// The innermost popup currently holding a grab, or NULL.
///
/// Surfaces are head-inserted, so the first match walking forward is the most
/// recently created one — the innermost menu level.
static struct np_surface *grabbing_popup(struct np_server *server) {
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->has_grab) return surface;
	}
	return NULL;
}

static void set_keyboard_focus(struct np_server *server, uint32_t window_id) {
	if (server->focused_window == window_id) return;

	struct np_surface *previous = server->focused_window
		? surface_by_window(server, server->focused_window) : NULL;
	struct np_surface *next = window_id ? surface_by_window(server, window_id) : NULL;
	struct np_input *entry;
	uint32_t serial = wl_display_next_serial(server->display);

	if (previous) {
		wl_list_for_each(entry, &server->keyboards, link) {
			if (!same_client(entry->resource, previous->resource)) continue;
			wl_keyboard_send_leave(entry->resource, serial, previous->resource);
		}
	}
	if (next) {
		struct wl_array keys;
		wl_array_init(&keys);
		// The selection belongs to the keyboard-focused client. The core
		// protocol requires this event immediately before keyboard.enter, not
		// while an unfocused client is merely constructing its data device.
		wl_list_for_each(entry, &server->data_devices, link) {
			if (!same_client(entry->resource, next->resource)) continue;
			send_selection_to(server, entry->resource);
		}
		int delivered = 0;
		wl_list_for_each(entry, &server->keyboards, link) {
			if (!same_client(entry->resource, next->resource)) continue;
			wl_keyboard_send_enter(entry->resource, serial, next->resource, &keys);
			delivered++;
		}
		wl_array_release(&keys);
		if (trace_enabled()) {
			fprintf(stderr, "[wayland] focus -> window %u surface %u, %d keyboards\n",
			        window_id, next->id, delivered);
		}
	} else if (trace_enabled()) {
		fprintf(stderr, "[wayland] focus -> window %u: no such surface\n", window_id);
	}
	server->focused_window = window_id;
	text_input_focus_changed(server, previous, next);
}

/// Wayland has no "modifiers changed" input event separate from the key that
/// changed them, so the current set is pushed alongside every key.
static void send_modifiers(struct np_server *server, struct np_surface *surface,
                           uint32_t modifiers) {
	uint32_t depressed = 0;
	if (modifiers & (1u << 0)) depressed |= 1u << 0;  // shift
	if (modifiers & (1u << 4)) depressed |= 1u << 1;  // caps lock
	if (modifiers & (1u << 1)) depressed |= 1u << 2;  // control
	if (modifiers & (1u << 2)) depressed |= 1u << 3;  // alt
	if (modifiers & (1u << 3)) depressed |= 1u << 6;  // super

	struct np_input *entry;
	uint32_t serial = wl_display_next_serial(server->display);
	wl_list_for_each(entry, &server->keyboards, link) {
		if (!same_client(entry->resource, surface->resource)) continue;
		wl_keyboard_send_modifiers(entry->resource, serial, depressed, 0, 0, 0);
	}
}

static void handle_pointer_position(struct np_server *server, uint32_t window_id,
                                    wl_fixed_t x, wl_fixed_t y) {
	struct np_surface *root = surface_by_window(server, window_id);
	if (!root) return;
	server->pointer_x = wl_fixed_to_double(x);
	server->pointer_y = wl_fixed_to_double(y);
	double local_x = server->pointer_x, local_y = server->pointer_y;
	struct np_surface *surface = np_scene_hit_test(
		root, server->pointer_x, server->pointer_y, &local_x, &local_y);
	if (!surface) surface = root;
	wl_fixed_t sx = fixed_from(local_x), sy = fixed_from(local_y);
	bool entering = server->pointer_surface != surface->id;

	struct np_input *entry;
	uint32_t serial = wl_display_next_serial(server->display);
	uint32_t time = now_ms();
	if (server->drag_source && !server->drag_dropped) {
		if (server->drag_focus_surface != surface->id) {
			if (server->drag_focus_window) drag_send_leave(server, server->drag_focus_window);
			drag_send_enter(server, surface, sx, sy);
		} else {
			drag_send_motion(server, surface, time, sx, sy);
		}
		wl_display_flush_clients(server->display);
		return;
	}

	// Tracking areas belonging to two NSWindows can overlap briefly while a
	// child window is ordered. Repair that host ordering to Wayland's single
	// pointer-focus, leave-before-enter model.
	if (entering && server->pointer_surface) {
		struct np_surface *previous = surface_by_id(server, server->pointer_surface);
		if (previous) {
			wl_list_for_each(entry, &server->pointers, link) {
				if (!same_client(entry->resource, previous->resource)) continue;
				wl_pointer_send_leave(entry->resource, serial, previous->resource);
				pointer_frame(entry->resource);
			}
		}
	}
	wl_list_for_each(entry, &server->pointers, link) {
		if (!same_client(entry->resource, surface->resource)) continue;
		if (entering) wl_pointer_send_enter(entry->resource, serial, surface->resource, sx, sy);
		else wl_pointer_send_motion(entry->resource, time, sx, sy);
		pointer_frame(entry->resource);
	}
	server->pointer_window = window_id;
	server->pointer_surface = surface->id;
	wl_display_flush_clients(server->display);
}

static uint32_t read_le32(const unsigned char *bytes) {
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_le64(const unsigned char *bytes) {
	return (uint64_t)read_le32(bytes) | ((uint64_t)read_le32(bytes + 4) << 32);
}

static void handle_pointer_scroll(struct np_server *server, uint32_t window_id,
                                  double dx, double dy) {
	struct np_surface *surface = surface_by_id(server, server->pointer_surface);
	if (!surface || server->pointer_window != window_id)
		surface = surface_by_window(server, window_id);
	if (!surface) return;
	struct np_input *entry;
	uint32_t time = now_ms();
	wl_list_for_each(entry, &server->pointers, link) {
		if (!same_client(entry->resource, surface->resource)) continue;
		if (dy != 0) {
			wl_pointer_send_axis(entry->resource, time,
			                     WL_POINTER_AXIS_VERTICAL_SCROLL, fixed_from(dy));
		}
		if (dx != 0) {
			wl_pointer_send_axis(entry->resource, time,
			                     WL_POINTER_AXIS_HORIZONTAL_SCROLL, fixed_from(dx));
		}
		pointer_frame(entry->resource);
	}
	wl_display_flush_clients(server->display);
}

/// Binary host-control path. High-rate state and frame feedback never allocate
/// a cJSON object; the outer NPIP framing still supplies message boundaries.
static void handle_host_binary(const unsigned char *payload, size_t length, void *user_data) {
	struct np_server *server = user_data;
	if (length == 16 && memcmp(payload, "NPMO", 4) == 0) {
		uint32_t window_id = read_le32(payload + 4);
		wl_fixed_t x = (wl_fixed_t)(int32_t)read_le32(payload + 8);
		wl_fixed_t y = (wl_fixed_t)(int32_t)read_le32(payload + 12);
		handle_pointer_position(server, window_id, x, y);
		return;
	}
	if (length == 28 && memcmp(payload, "NPSC", 4) == 0) {
		uint64_t dx_bits = read_le64(payload + 8);
		uint64_t dy_bits = read_le64(payload + 16);
		double dx, dy;
		memcpy(&dx, &dx_bits, sizeof(dx));
		memcpy(&dy, &dy_bits, sizeof(dy));
		handle_pointer_scroll(server, read_le32(payload + 4), dx, dy);
		return;
	}
	if (length == 24 && memcmp(payload, "NPCF", 4) == 0) {
		uint32_t window_id = read_le32(payload + 4);
		struct np_surface *surface = surface_by_window(server, window_id);
		if (!surface || !surface->toplevel) return;
		int32_t width = (int32_t)read_le32(payload + 8);
		int32_t height = (int32_t)read_le32(payload + 12);
		uint32_t state_bits = read_le32(payload + 16);
		/* payload + 20 is the host diagnostic serial. xdg-shell owns the
		 * independent serial generated by send_host_toplevel_configure(). */
		queue_host_toplevel_configure(surface, width, height, state_bits);
		wl_display_flush_clients(server->display);
		return;
	}
	if (length >= 8 && memcmp(payload, "NPFT", 4) == 0) {
		uint32_t count = read_le32(payload + 4);
		if ((length - 8) % 12 != 0 || (size_t)count != (length - 8) / 12)
			return;
		for (uint32_t i = 0; i < count; i++) {
			const unsigned char *record = payload + 8 + (size_t)i * 12;
			uint32_t kind = read_le32(record);
			uint32_t surface_id = read_le32(record + 4);
			uint32_t presentation_id = read_le32(record + 8);
			if (kind == 1)
				process_frame_presented(server, surface_id, presentation_id);
			else if (kind == 2)
				process_frame_released(server, surface_id, presentation_id);
		}
		finish_frame_feedback(server);
	}
}

static void handle_host_command(const char *name, cJSON *body, void *user_data) {
	struct np_server *server = user_data;
	if (trace_enabled() && strcmp(name, "pointerMoved") != 0 &&
	    strcmp(name, "pointerScroll") != 0 && strcmp(name, "configure") != 0) {
		char *text = cJSON_PrintUnformatted(body);
		fprintf(stderr, "[wayland] <- %s %s\n", name, text ? text : "");
		free(text);
	}

	if (strcmp(name, "framePresented") == 0) {
		uint32_t surface_id = (uint32_t)json_int(body, "surface", 0);
		uint32_t presentation_id = (uint32_t)json_int(body, "presentationID", 0);
		process_frame_presented(server, surface_id, presentation_id);
		finish_frame_feedback(server);
		return;
	}

	if (strcmp(name, "frameReleased") == 0) {
		uint32_t surface_id = (uint32_t)json_int(body, "surface", 0);
		uint32_t presentation_id = (uint32_t)json_int(body, "presentationID", 0);
		process_frame_released(server, surface_id, presentation_id);
		finish_frame_feedback(server);
		return;
	}

	if (strcmp(name, "configure") == 0) {
		uint32_t window_id = (uint32_t)json_int(body, "window", 0);
		struct np_surface *surface = surface_by_window(server, window_id);
		if (!surface || !surface->toplevel) return;

		cJSON *size = cJSON_GetObjectItemCaseSensitive(body, "size");
		int width = size ? json_int(size, "width", 0) : 0;
		int height = size ? json_int(size, "height", 0) : 0;

		uint32_t state_bits = 0;
		cJSON *state_list = cJSON_GetObjectItemCaseSensitive(body, "states");
		cJSON *state;
		cJSON_ArrayForEach(state, state_list) {
			if (!cJSON_IsString(state)) continue;
			if (strcmp(state->valuestring, "maximized") == 0)
				state_bits |= NP_CONFIGURE_MAXIMIZED;
			else if (strcmp(state->valuestring, "fullscreen") == 0)
				state_bits |= NP_CONFIGURE_FULLSCREEN;
			else if (strcmp(state->valuestring, "resizing") == 0)
				state_bits |= NP_CONFIGURE_RESIZING;
			else if (strcmp(state->valuestring, "activated") == 0)
				state_bits |= NP_CONFIGURE_ACTIVATED;
		}
		queue_host_toplevel_configure(surface, width, height, state_bits);
		// Flushed before allocating: the client starts repainting now, and the
		// map round trip overlaps that instead of following it.
		wl_display_flush_clients(server->display);

		return;
	}

	if (strcmp(name, "close") == 0) {
		struct np_surface *surface = surface_by_window(server, (uint32_t)json_int(body, "window", 0));
		if (surface && surface->toplevel) {
			// A request, not an order: the client decides whether to exit.
			xdg_toplevel_send_close(surface->toplevel);
			wl_display_flush_clients(server->display);
		}
		return;
	}

	if (strcmp(name, "dismissPopup") == 0) {
		struct np_surface *surface = surface_by_window(server, (uint32_t)json_int(body, "window", 0));
		if (surface && surface->popup) {
			xdg_popup_send_popup_done(surface->popup);
			wl_display_flush_clients(server->display);
		}
		return;
	}

	if (strcmp(name, "textCommit") == 0) {
		cJSON *text = cJSON_GetObjectItemCaseSensitive(body, "text");
		text_input_deliver(server, cJSON_IsString(text) ? text->valuestring : "",
		                   // An empty preedit alongside the commit is what ends
		                   // the composition the commit came out of.
		                   "", 0, 0, 0, 0);
		return;
	}

	if (strcmp(name, "textPreedit") == 0) {
		cJSON *text = cJSON_GetObjectItemCaseSensitive(body, "text");
		text_input_deliver(server, NULL, cJSON_IsString(text) ? text->valuestring : "",
		                   (int32_t)json_int(body, "cursorBegin", 0),
		                   (int32_t)json_int(body, "cursorEnd", 0), 0, 0);
		return;
	}

	if (strcmp(name, "textDeleteSurrounding") == 0) {
		text_input_deliver(server, NULL, NULL, 0, 0,
		                   (int32_t)json_int(body, "beforeLength", 0),
		                   (int32_t)json_int(body, "afterLength", 0));
		return;
	}

	if (strcmp(name, "selectionRequest") == 0) {
		cJSON *mime = cJSON_GetObjectItemCaseSensitive(body, "mimeType");
		clip_serve_host_request(server, (uint32_t)json_int(body, "token", 0),
		                        cJSON_IsString(mime) ? mime->valuestring : "");
		return;
	}

	if (strcmp(name, "hostSelectionOffered") == 0) {
		// The Mac now owns the clipboard, which retires whatever guest client
		// held it. Telling that client it was cancelled is what lets it drop the
		// data it was holding for a paste that will never come.
		for (int i = 0; i < server->host_mime_count; i++) free(server->host_mime[i]);
		server->host_mime_count = 0;
		cJSON *types = cJSON_GetObjectItemCaseSensitive(body, "mimeTypes");
		cJSON *type;
		cJSON_ArrayForEach(type, types) {
			if (!cJSON_IsString(type)) continue;
			if (server->host_mime_count >= (int)(sizeof(server->host_mime) /
			                                     sizeof(server->host_mime[0]))) break;
			server->host_mime[server->host_mime_count++] = strdup(type->valuestring);
		}
		if (server->host_mime_count > 0 && server->selection_source) {
			wl_data_source_send_cancelled(server->selection_source);
			server->selection_source = NULL;
		}
		struct np_surface *focused = server->focused_window
			? surface_by_window(server, server->focused_window) : NULL;
		if (focused) {
			struct np_input *device;
			wl_list_for_each(device, &server->data_devices, link) {
				if (wl_resource_get_client(device->resource) !=
				    wl_resource_get_client(focused->resource)) continue;
				send_selection_to(server, device->resource);
			}
		}
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "hostSelectionData") == 0) {
		cJSON *encoded = cJSON_GetObjectItemCaseSensitive(body, "base64");
		clip_deliver_host_data(server, (uint32_t)json_int(body, "token", 0),
		                       cJSON_IsString(encoded) ? encoded->valuestring : NULL);
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "keyboardFocus") == 0) {
		// A grab outranks the host's view of which NSWindow is key. Without this
		// the parent window, which never stopped being key, would immediately
		// take focus back off the menu.
		if (grabbing_popup(server)) return;
		set_keyboard_focus(server, (uint32_t)json_int(body, "window", 0));
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "key") == 0) {
		struct np_surface *surface = surface_by_window(server, (uint32_t)json_int(body, "window", 0));
		if (!surface) return;
		uint32_t keycode = (uint32_t)json_int(body, "keycode", 0);
		bool pressed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "pressed"));
		send_modifiers(server, surface, (uint32_t)json_int(body, "modifiers", 0));

		struct np_input *entry;
		uint32_t serial = wl_display_next_serial(server->display);
		uint32_t time = now_ms();
		int delivered = 0, total = 0;
		wl_list_for_each(entry, &server->keyboards, link) {
			total++;
			if (!same_client(entry->resource, surface->resource)) continue;
			wl_keyboard_send_key(entry->resource, serial, time, keycode,
			                     pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			                             : WL_KEYBOARD_KEY_STATE_RELEASED);
			delivered++;
		}
		if (trace_enabled()) {
			fprintf(stderr, "[wayland] key %u -> %d of %d keyboards (surface %u)\n",
			        keycode, delivered, total, surface->id);
		}
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "pointerEntered") == 0 || strcmp(name, "pointerMoved") == 0) {
		uint32_t window_id = (uint32_t)json_int(body, "window", 0);
		wl_fixed_t x = fixed_from(json_double(body, "x", 0));
		wl_fixed_t y = fixed_from(json_double(body, "y", 0));
		handle_pointer_position(server, window_id, x, y);
		return;
	}

	if (strcmp(name, "pointerLeft") == 0) {
		uint32_t window_id = (uint32_t)json_int(body, "window", 0);
		if (server->drag_source && !server->drag_dropped &&
		    server->drag_focus_window == window_id) {
			drag_send_leave(server, window_id);
			server->drag_focus_window = 0;
		}
		// Ignore a delayed exit from the window that lost focus during the
		// leave-before-enter repair above.
		if (server->pointer_window != window_id) return;
		struct np_surface *surface = surface_by_id(server, server->pointer_surface);
		if (!surface) surface = surface_by_window(server, window_id);
		if (!surface) return;
		struct np_input *entry;
		uint32_t serial = wl_display_next_serial(server->display);
		wl_list_for_each(entry, &server->pointers, link) {
			if (!same_client(entry->resource, surface->resource)) continue;
			wl_pointer_send_leave(entry->resource, serial, surface->resource);
			pointer_frame(entry->resource);
		}
		server->pointer_window = 0;
		server->pointer_surface = 0;
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "pointerButton") == 0) {
		uint32_t window_id = (uint32_t)json_int(body, "window", 0);
		struct np_surface *surface = server->pointer_window == window_id
			? surface_by_id(server, server->pointer_surface) : NULL;
		if (!surface) surface = surface_by_window(server, window_id);
		if (!surface) return;
		cJSON *which = cJSON_GetObjectItemCaseSensitive(body, "button");
		uint32_t code = BTN_LEFT;
		if (cJSON_IsString(which)) {
			if (strcmp(which->valuestring, "right") == 0) code = BTN_RIGHT;
			else if (strcmp(which->valuestring, "middle") == 0) code = BTN_MIDDLE;
		}
		bool pressed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "pressed"));
		if (!pressed && server->drag_source && !server->drag_dropped) {
			drag_finish_on_button_release(server);
			wl_display_flush_clients(server->display);
			return;
		}

		struct np_input *entry;
		uint32_t serial = wl_display_next_serial(server->display);
		uint32_t time = now_ms();
		wl_list_for_each(entry, &server->pointers, link) {
			if (!same_client(entry->resource, surface->resource)) continue;
			wl_pointer_send_button(entry->resource, serial, time, code,
			                       pressed ? WL_POINTER_BUTTON_STATE_PRESSED
			                               : WL_POINTER_BUTTON_STATE_RELEASED);
			pointer_frame(entry->resource);
		}
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "pointerScroll") == 0) {
		handle_pointer_scroll(
			server, (uint32_t)json_int(body, "window", 0),
			json_double(body, "dx", 0), json_double(body, "dy", 0));
		return;
	}

	if (strcmp(name, "scaleChanged") == 0) {
		int scale = json_int(body, "scale", 0);
		if (scale <= 0) return;

		// Per surface, because that is what the host is telling us: this window
		// is now on a display of this density. Applying it to wl_output would
		// change every window at once, which is wrong the moment two of them sit
		// on screens that differ.
		struct np_surface *surface = surface_by_window(server, (uint32_t)json_int(body, "window", 0));
		if (surface) np_scale_changed(surface, scale);

		// wl_output still carries a scale for clients too old to know about
		// wp_fractional_scale_v1; the primary display's is the best single
		// answer available.
		np_scale_update_output(server, scale);
		wl_display_flush_clients(server->display);
		return;
	}
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void sync_host_channels(struct np_server *server);

static int host_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&server->host);
	np_host_pump(&server->host, handle_host_command, handle_host_binary, server);
	sync_host_channels(server);
	flush_pending_frames(server);
	wl_display_flush_clients(server->display);
	return 0;
}

#ifndef NP_REMOTE
static int host_control_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&server->host_control);
	np_host_pump(&server->host_control, handle_host_command, handle_host_binary, server);
	sync_host_channels(server);
	return 0;
}

static int host_input_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&server->host_input);
	np_host_pump(&server->host_input, handle_host_command, handle_host_binary, server);
	sync_host_channels(server);
	return 0;
}

static int host_feedback_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&server->host_feedback);
	np_host_pump(&server->host_feedback, handle_host_command, handle_host_binary, server);
	sync_host_channels(server);
	return 0;
}
#endif

/// Re-announces every live surface to a host that has just attached.
///
/// The host keeps no window state across a disconnect, and a client that is
/// merely idle will not commit again to prompt one. Without this the redial
/// succeeds and the windows still never come back, which makes the reconnect
/// path look like recovery without being it.
static void republish_state(struct np_server *server) {
	struct np_surface *surface;

	// Surfaces first, and in creation order: a role or a parent reference is
	// meaningless to the host until the surface it names exists.
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		np_host_send(&server->host, "surfaceCreated",
		             object_with_u32("surface", surface->id));
	}

	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		cJSON *body;
		if (surface->toplevel) {
			body = cJSON_CreateObject();
			cJSON_AddNumberToObject(body, "window", surface->window_id);
			cJSON_AddNumberToObject(body, "surface", surface->id);
			np_host_send(&server->host, "toplevelCreated", body);
		} else if (surface->popup) {
			body = cJSON_CreateObject();
			cJSON_AddNumberToObject(body, "window", surface->window_id);
			cJSON_AddNumberToObject(body, "surface", surface->id);
			cJSON_AddNumberToObject(body, "parent",
			                        surface->parent ? surface->parent->window_id : 0);
			cJSON_AddNumberToObject(body, "x", surface->sub_x);
			cJSON_AddNumberToObject(body, "y", surface->sub_y);
			cJSON_AddNumberToObject(body, "width", surface->last_width);
			cJSON_AddNumberToObject(body, "height", surface->last_height);
			np_host_send(&server->host, "popupCreated", body);
		}

		if (surface->title) {
			body = cJSON_CreateObject();
			cJSON_AddNumberToObject(body, "window", surface->window_id);
			cJSON_AddStringToObject(body, "title", surface->title);
			np_host_send(&server->host, "titleChanged", body);
		}
		if (surface->app_id) {
			body = cJSON_CreateObject();
			cJSON_AddNumberToObject(body, "window", surface->window_id);
			cJSON_AddStringToObject(body, "appID", surface->app_id);
			np_host_send(&server->host, "appIDChanged", body);
		}
		if (surface->decoration_negotiated) {
			body = cJSON_CreateObject();
			cJSON_AddNumberToObject(body, "window", surface->window_id);
			cJSON_AddBoolToObject(body, "serverSide", surface->decoration_server_side);
			np_host_send(&server->host, "decorationModeChanged", body);
		}
	}

#ifndef NP_REMOTE
	/* Rebuild each current xdg scene from its retained source textures. Replaying
	 * the old one-surface `committed` envelope would bypass subsurface ordering,
	 * viewport/clip state and the new host renderer. All callbacks and FIFO
	 * barriers outstanding from the lost host are rebound to this new latch. */
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		struct np_surface *root = np_scene_root(surface);
		if (root != surface || !surface->has_published ||
		    (!surface->current_gpu && !surface->current_shm))
			continue;
		uint32_t presentation_id = next_presentation_id(server);
		struct np_surface *member;
		wl_list_for_each(member, &server->surfaces, link) {
			if (np_scene_root(member) != root) continue;
			struct np_frame_callback *callback;
			wl_list_for_each(callback, &member->pending_frame_callbacks, link) {
				if (callback->presentation_id)
					callback->presentation_id = presentation_id;
			}
			if (member->fifo_barrier_active)
				member->fifo_barrier_presentation_id = presentation_id;
		}
		root->scene_presentation_id = presentation_id;
		root->scene_dirty = true;
	}

	/* Cursor and drag-icon surfaces have no xdg root, so they retain the small
	 * metadata envelope. queue_last_published_frame establishes a new host-read
	 * hold for the retained texture before it is sent. */
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		if (np_scene_root(surface) || !surface->has_published ||
		    (!surface->current_gpu && !surface->current_shm))
			continue;
		uint32_t presentation_id = next_presentation_id(server);
		struct np_frame_callback *callback;
		wl_list_for_each(callback, &surface->pending_frame_callbacks, link) {
			if (callback->presentation_id)
				callback->presentation_id = presentation_id;
		}
		if (surface->fifo_barrier_active)
			surface->fifo_barrier_presentation_id = presentation_id;
		(void)queue_last_published_frame(surface, presentation_id);
	}
#else
	// Remote output is still an encoded per-surface stream, so replay its latest
	// decoded frame through the ordinary committed envelope.
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		if (!surface->has_published) continue;
		uint32_t resource_id = surface->last_resource_id;
		if (!resource_id) continue;

		cJSON *frame = cJSON_CreateObject();
		cJSON_AddNumberToObject(frame, "resourceID", resource_id);
		cJSON_AddNumberToObject(frame, "width", surface->last_width);
		cJSON_AddNumberToObject(frame, "height", surface->last_height);
		cJSON_AddNumberToObject(frame, "bytesPerRow", surface->last_stride);
		cJSON_AddStringToObject(frame, "format", surface->last_format);
		if (surface->last_source) {
			cJSON_AddStringToObject(frame, "source", surface->last_source);
		}
		if (surface->last_source && strcmp(surface->last_source, "encoded") == 0) {
			cJSON_AddStringToObject(frame, "codec", "h264");
			cJSON_AddNumberToObject(frame, "bitstreamEpoch", surface->last_epoch);
		}
		cJSON_AddNumberToObject(frame, "scale", surface->scale);
		if (surface->geometry_set) {
			cJSON *geometry = cJSON_CreateObject();
			cJSON_AddNumberToObject(geometry, "x", surface->geometry_x);
			cJSON_AddNumberToObject(geometry, "y", surface->geometry_y);
			cJSON_AddNumberToObject(geometry, "width", surface->geometry_width);
			cJSON_AddNumberToObject(geometry, "height", surface->geometry_height);
			cJSON_AddItemToObject(frame, "windowGeometry", geometry);
		}
		frame_add_viewport(surface, frame);
		cJSON_AddItemToObject(frame, "damage", cJSON_CreateArray());

		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "surface", surface->id);
		cJSON_AddItemToObject(body, "frame", frame);

		// Straight into the pending slot, so a commit racing this replay wins.
		if (surface->pending_frame) cJSON_Delete(surface->pending_frame);
		surface->pending_frame = body;
	}
#endif
}

static void discard_disconnected_host_reads(struct np_server *server) {
#ifndef NP_REMOTE
	struct np_surface *surface;
	/* No completion can arrive from the old socket. Releasing these holds is
	 * safe because that host can no longer submit new Metal work from them. */
	wl_list_for_each(surface, &server->surfaces, link)
		np_scene_discard_presentations(surface);
	wl_list_for_each(surface, &server->surfaces, link)
		apply_unblocked_updates(surface);
#else
	(void)server;
#endif
}

static void sync_one_host_source(
	struct np_server *server, struct np_host *host,
	struct wl_event_source **source, int *watched_fd, uint32_t *watched_mask,
	wl_event_loop_fd_func_t callback)
{
	/* Writability is watched only while this lane has queued bytes. Each lane has
	 * independent vsock credit, so a blocked feedback stream cannot park control
	 * or input behind the same POLLOUT wait. */
	uint32_t mask = WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR;
	if (np_host_has_backlog(host)) mask |= WL_EVENT_WRITABLE;

	if (*watched_fd == host->conn_fd) {
		if (*source && *watched_mask != mask) {
			wl_event_source_fd_update(*source, mask);
			*watched_mask = mask;
		}
		return;
	}
	if (*source) {
		wl_event_source_remove(*source);
		*source = NULL;
	}
	*watched_fd = host->conn_fd;
	*watched_mask = mask;
	if (host->conn_fd >= 0) {
		struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
		*source = wl_event_loop_add_fd(
			loop, host->conn_fd, mask, callback, server);
	}
}

static bool all_host_channels_connected(struct np_server *server)
{
	if (!np_host_connected(&server->host)) return false;
#ifndef NP_REMOTE
	return np_host_connected(&server->host_control) &&
	       np_host_connected(&server->host_input) &&
	       np_host_connected(&server->host_feedback);
#else
	return true;
#endif
}

static void close_host_session(struct np_server *server)
{
	np_host_disconnect(&server->host);
#ifndef NP_REMOTE
	np_host_disconnect(&server->host_control);
	np_host_disconnect(&server->host_input);
	np_host_disconnect(&server->host_feedback);
#endif
}

static void sync_host_channels(struct np_server *server) {
	bool connected = all_host_channels_connected(server);
	if (server->host_session_ready && !connected) {
		server->host_session_ready = false;
#ifndef NP_REMOTE
		unpublish_session_environment();
#endif
		discard_disconnected_host_reads(server);
		/* Do not pair a newly dialled lane with sockets from the old generation. */
		close_host_session(server);
		connected = false;
	}

	if (!server->host_session_ready && connected) {
		/* channelReady is the host launch gate. The environment must be visible
		 * before the event because the host may launch immediately on receipt. */
#ifndef NP_REMOTE
		if (!publish_session_environment(server->session_socket))
			fprintf(stderr, "[wayland] could not publish the session environment\n");
#endif
		server->host_session_ready = true;
		cJSON *ready = cJSON_CreateObject();
		cJSON_AddNumberToObject(ready, "sessionID", (double)(uint32_t)getpid());
#ifdef NP_REMOTE
		cJSON_AddNumberToObject(ready, "protocolVersion", 1);
#else
		cJSON_AddNumberToObject(ready, "protocolVersion", 2);
#endif
		np_host_send(&server->host, "channelReady", ready);
		republish_state(server);
	}

	sync_one_host_source(
		server, &server->host, &server->host_connection_source,
		&server->watched_host_fd, &server->watched_host_mask,
		host_channel_readable);
#ifndef NP_REMOTE
	sync_one_host_source(
		server, &server->host_control, &server->host_control_connection_source,
		&server->watched_host_control_fd, &server->watched_host_control_mask,
		host_control_channel_readable);
	sync_one_host_source(
		server, &server->host_input, &server->host_input_connection_source,
		&server->watched_host_input_fd, &server->watched_host_input_mask,
		host_input_channel_readable);
	sync_one_host_source(
		server, &server->host_feedback, &server->host_feedback_connection_source,
		&server->watched_host_feedback_fd, &server->watched_host_feedback_mask,
		host_feedback_channel_readable);
#endif
}

static int host_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&server->host, handle_host_command, handle_host_binary, server);
	sync_host_channels(server);
	flush_pending_frames(server);
	wl_display_flush_clients(server->display);
	return 0;
}

#ifndef NP_REMOTE
static int host_control_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&server->host_control, handle_host_command, handle_host_binary, server);
	sync_host_channels(server);
	return 0;
}

static int host_input_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&server->host_input, handle_host_command, handle_host_binary, server);
	sync_host_channels(server);
	return 0;
}

static int host_feedback_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&server->host_feedback, handle_host_command, handle_host_binary, server);
	sync_host_channels(server);
	return 0;
}
#endif

#ifdef NP_REMOTE
static int media_listener_readable(int fd, uint32_t mask, void *data) {
	(void)fd;
	(void)mask;
	struct np_server *server = data;
	np_media_accept(&server->media);
	return 0;
}
#endif

int np_compositor_run(int argc, char **argv) {
	struct np_server server;
	memset(&server, 0, sizeof(server));
	server.next_id = 1;
	server.output_scale = 2;
	server.output_width = 3024;
	server.output_height = 1964;
	wl_list_init(&server.surfaces);
	wl_list_init(&server.shm_textures);
	wl_list_init(&server.outputs);
	wl_list_init(&server.pointers);
	wl_list_init(&server.keyboards);
	wl_list_init(&server.data_devices);
	wl_list_init(&server.data_offers);
	wl_list_init(&server.clip_reads);
	wl_list_init(&server.clip_writes);
	wl_list_init(&server.text_inputs);
	// Clipboard transfers write into pipes owned by other processes, and a
	// client that closes its end mid-paste would otherwise take the compositor
	// down with it.
	signal(SIGPIPE, SIG_IGN);
	server.keymap_fd = -1;
	server.watched_host_fd = -1;
#ifndef NP_REMOTE
	server.watched_host_control_fd = -1;
	server.watched_host_input_fd = -1;
	server.watched_host_feedback_fd = -1;
#endif
	g_server = &server;
	(void)argc;
	(void)argv;

#ifndef NP_REMOTE
	/* A compositor killed before its host socket closes cannot remove its
	 * readiness record.  Never let a replacement Wayland socket inherit that
	 * stale launch permission. */
	unpublish_session_environment();
#endif

	server.drm_fd = -1;
#ifdef NP_REMOTE
	fprintf(stderr, "[wayland] remote build: TCP 1025/1026, H.264 encode, no virtio blobs\n");
#else
	server.drm_fd = np_virtio_open_lookup_node();
	if (server.drm_fd < 0) {
		fprintf(stderr, "[wayland] no virtio-gpu render node; cannot allocate host buffers\n");
		return 1;
	}
#endif

	server.display = wl_display_create();
	if (!server.display) {
		fprintf(stderr, "[wayland] wl_display_create failed\n");
		return 1;
	}

	// libwayland's own wl_shm implementation. Nothing is gained by writing
	// another one, and it hands us wl_shm_buffer directly.
	if (wl_display_init_shm(server.display) < 0) {
		fprintf(stderr, "[wayland] wl_display_init_shm failed\n");
		return 1;
	}

	wl_global_create(server.display, &wl_compositor_interface, COMPOSITOR_VERSION,
	                 &server, compositor_bind);
	wl_global_create(server.display, &wl_subcompositor_interface, 1, &server, subcompositor_bind);
	wl_global_create(server.display, &wl_data_device_manager_interface, 3,
	                 &server, data_manager_bind);
	wl_global_create(server.display, &xdg_wm_base_interface, XDG_WM_BASE_VERSION,
	                 &server, wm_base_bind);
	wl_global_create(server.display, &wl_seat_interface, SEAT_VERSION, &server, seat_bind);
	np_scale_advertise(server.display, &server);
	wl_global_create(server.display, &wp_fifo_manager_v1_interface, 1,
	                 &server, fifo_manager_bind);
	wl_global_create(server.display, &zwp_text_input_manager_v3_interface, 1,
	                 &server, text_input_manager_bind);
	np_decoration_advertise(server.display, &server);
#ifndef NP_REMOTE
	np_dmabuf_advertise(server.display, server.drm_fd);
	np_syncobj_advertise(server.display, &server);
#endif

	/* The environment file below is the application-launch readiness record.
	 * Establish the host transport first so a client cannot observe a live
	 * Wayland socket while window events still have nowhere to go. */
#ifdef NP_REMOTE
	if (!np_host_listen_tcp(&server.host, NP_SURFACE_PORT)) return 1;
	if (!np_media_listen(&server.media)) return 1;
#else
	if (!np_host_listen(&server.host, NP_SURFACE_PORT) ||
	    !np_host_listen(&server.host_control, NP_WINDOW_CONTROL_PORT) ||
	    !np_host_listen(&server.host_input, NP_WINDOW_INPUT_PORT) ||
	    !np_host_listen(&server.host_feedback, NP_WINDOW_FEEDBACK_PORT))
		return 1;
#endif

	const char *socket = wl_display_add_socket_auto(server.display);
	if (!socket) {
		fprintf(stderr, "[wayland] could not create a Wayland socket\n");
		return 1;
	}
	fprintf(stderr, "[wayland] WAYLAND_DISPLAY=%s\n", socket);
#ifndef NP_REMOTE
	if (strlen(socket) >= sizeof(server.session_socket)) {
		fprintf(stderr, "[wayland] display name is too long\n");
		return 1;
	}
	strcpy(server.session_socket, socket);
#endif
#ifdef NP_REMOTE
	// So `remotepipe user@host` (and any later shell) can point clients at
	// *this* compositor even when the session already owns wayland-0 under
	// the normal XDG_RUNTIME_DIR.
	{
		const char *runtime = getenv("XDG_RUNTIME_DIR");
		if (!runtime || !runtime[0]) runtime = "/tmp";
		FILE *env = fopen("/tmp/remotepipe-wayland.env", "w");
		if (env) {
			fprintf(env, "WAYLAND_DISPLAY=%s\nXDG_RUNTIME_DIR=%s\n", socket, runtime);
			fclose(env);
		}
	}
#endif

	if (!create_keymap(&server)) {
		fprintf(stderr, "[wayland] no keymap; keyboard input will not work\n");
	}

	// The host channel is a file descriptor like any other, so it belongs in the
	// same event loop as the Wayland clients. Polling it after each dispatch made
	// the two starve each other: a slow Wayland turn delayed every host command,
	// and an idle loop still woke up sixty times a second.
	struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
	wl_event_loop_add_fd(loop, server.host.listen_fd, WL_EVENT_READABLE,
	                     host_listener_readable, &server);
#ifndef NP_REMOTE
	wl_event_loop_add_fd(loop, server.host_control.listen_fd, WL_EVENT_READABLE,
	                     host_control_listener_readable, &server);
	wl_event_loop_add_fd(loop, server.host_input.listen_fd, WL_EVENT_READABLE,
	                     host_input_listener_readable, &server);
	wl_event_loop_add_fd(loop, server.host_feedback.listen_fd, WL_EVENT_READABLE,
	                     host_feedback_listener_readable, &server);
#endif
#ifdef NP_REMOTE
	if (server.media.listen_fd >= 0) {
		wl_event_loop_add_fd(loop, server.media.listen_fd, WL_EVENT_READABLE,
		                     media_listener_readable, &server);
	}
#endif
	for (;;) {
		flush_pending_frames(&server);
		wl_display_flush_clients(server.display);
		wl_event_loop_dispatch(loop, -1);
		np_host_pump(&server.host, handle_host_command, handle_host_binary, &server);
#ifndef NP_REMOTE
		np_host_pump(&server.host_control, handle_host_command, handle_host_binary, &server);
		np_host_pump(&server.host_input, handle_host_command, handle_host_binary, &server);
		np_host_pump(&server.host_feedback, handle_host_command, handle_host_binary, &server);
#endif
#ifdef NP_REMOTE
		np_media_pump(&server.media);
		np_media_accept(&server.media);
		if (server.media.just_attached) {
			server.media.just_attached = false;
			struct np_surface *surface;
			wl_list_for_each(surface, &server.surfaces, link) {
				if (surface->encoder) np_encoder_force_keyframe(surface->encoder);
			}
			fprintf(stderr, "[media] requested IDR on all encoders\n");
		}
#endif
		sync_host_channels(&server);
		flush_pending_frames(&server);
	}

#ifdef NP_REMOTE
	np_media_finish(&server.media);
#endif
	np_host_finish(&server.host);
#ifndef NP_REMOTE
	np_host_finish(&server.host_control);
	np_host_finish(&server.host_input);
	np_host_finish(&server.host_feedback);
#endif
	wl_display_destroy(server.display);
#ifndef NP_REMOTE
	if (server.drm_fd >= 0) close(server.drm_fd);
#endif
	return 0;
}
