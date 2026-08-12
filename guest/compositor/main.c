// nativepipe-wayland — the guest half of the window path.
//
// This is a *translator*, not a compositor. It runs the Wayland protocol state
// machine — surface roles, commit atomicity, buffer release timing — and
// forwards what macOS needs to put a window on screen. It deliberately has no
// scene graph, no stacking order, no damage accumulation, no decorations and no
// output composition: macOS already does all of that, and a second opinion would
// only have to be reconciled with the first.
//
//     wl_surface   #17  ->  NativeSurface #17
//     xdg_toplevel #23  ->  NativeWindow  #23  ->  NSWindow *
//
// Two buffer paths, both named by a virtio-gpu resource id:
//
//   * wl_shm  — one memcpy into a host IOSurface. That is the CPU window.
//   * linux-dmabuf — Mesa Venus already rendered on the host. We only
//     pass the resource id. The host presents that image on a CAMetalLayer.

#define _GNU_SOURCE

#include "blob.h"
#include "dmabuf.h"
#include "hostlink.h"
#include "fractional-scale-v1-server-protocol.h"
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

/// An empty box has width 0; a full-surface box is set by damage_full().
struct np_box {
	int32_t x, y, width, height;
};

struct np_server {
	struct wl_display *display;
	struct wl_list surfaces;  // np_surface.link
	struct np_host host;
	int drm_fd;
	uint32_t next_id;
	int output_scale;
	int output_width;
	int output_height;
	/// Bound wl_output resources, so a scale change reaches clients that already
	/// have one. Announcing it only at bind time would leave every existing
	/// window rendering at the old density.
	struct wl_list outputs;

	// Input. Devices are tracked per binding so events can be delivered only to
	// the client that owns the focused surface.
	struct wl_list pointers;
	struct wl_list keyboards;
	int keymap_fd;
	/// The current clipboard owner, and the data devices that need telling about
	/// it. Selection is per seat, and there is one seat.
	struct wl_resource *selection_source;
	struct wl_list data_devices;
	/// Set when the macOS pasteboard owns the selection instead of a guest
	/// client. The two are mutually exclusive, which is what makes "who owns the
	/// clipboard" a single question rather than a merge.
	char *host_mime[24];
	int host_mime_count;
	uint32_t next_clip_token;
	/// In-flight clipboard transfers. Both directions go through a pipe that is
	/// driven by the event loop rather than a blocking read or write: the peer
	/// on the other end is an unrelated process that may never read, and a
	/// clipboard must not be able to wedge the compositor.
	struct wl_list clip_reads;
	struct wl_list clip_writes;
	/// zwp_text_input_v3 bindings. Composition happens on the Mac, so the only
	/// state kept here is which client is asking for it.
	struct wl_list text_inputs;
	/// Active Wayland drag. Guest-to-guest DnD stays entirely in this compositor;
	/// a future NSPasteboard bridge is only needed when the pointer crosses into
	/// a native macOS application.
	struct wl_resource *drag_source;
	struct wl_resource *drag_origin;
	struct wl_resource *drag_icon;
	uint32_t drag_focus_window;
	bool drag_dropped;
	double pointer_x, pointer_y;
	size_t keymap_size;
	uint32_t focused_window;
	uint32_t pointer_window;
	/// The accepted host socket must be part of the Wayland event loop. Watching
	/// only listen_fd lets the host connect, but its later input commands cannot
	/// wake an idle compositor.
	struct wl_event_source *host_connection_source;
	int watched_host_fd;
	uint32_t watched_host_mask;
};

struct np_input {
	struct wl_list link;
	struct wl_resource *resource;
	struct np_server *server;
};

struct np_output {
	struct wl_list link;
	struct wl_resource *resource;
};

struct np_frame_callback {
	struct wl_resource *resource;
	struct wl_event_source *timer;
};

struct np_surface {
	struct wl_list link;
	struct np_server *server;
	struct wl_resource *resource;
	uint32_t id;

	// Wayland state is double buffered: nothing takes effect until commit.
	struct wl_resource *pending_buffer;
	bool pending_buffer_set;
	int pending_scale;
	int scale;

	// xdg_surface / xdg_toplevel, once the surface takes a role.
	struct wl_resource *xdg_surface;
	struct wl_resource *toplevel;
	struct wl_resource *popup;
	struct wl_resource *decoration;
	/// wp_fractional_scale_v1, if the client asked for one. Scale reaches a
	/// surface through this rather than through wl_output, because on macOS the
	/// density belongs to the window's current screen — two windows can differ.
	struct wl_resource *fractional_scale;
	int reported_scale;
	uint32_t window_id;
	uint32_t configure_serial;
	// xdg_surface window geometry is double-buffered and takes effect on the
	// next wl_surface.commit. It excludes CSD shadow/resize margins from the
	// wl_surface without changing the surface coordinate system itself.
	bool pending_geometry_set;
	int32_t pending_geometry_x, pending_geometry_y;
	int32_t pending_geometry_width, pending_geometry_height;
	bool geometry_set;
	int32_t geometry_x, geometry_y;
	int32_t geometry_width, geometry_height;

	// Two host allocations, alternated. One is being shown while the other is
	// written, which stops the guest's memcpy from racing CoreAnimation's read.
	// It also matters for a reason that is easy to miss: assigning the *same*
	// IOSurface to CALayer.contents again is not a change, so a single buffer
	// updates only when the compositor happens to redraw for another reason.
	struct np_blob blobs[2];
	int back;
	uint32_t blob_stride;
	uint32_t alloc_width;
	uint32_t alloc_height;

	// Subsurface role, if this surface has one. A subsurface is part of its
	// parent's contents rather than a window of its own.
	struct wl_resource *subsurface;
	struct np_surface *parent;
	int32_t sub_x, sub_y;
	/// Synchronised is the default: a commit is cached and takes effect when the
	/// parent commits, so a compound widget updates in one piece.
	bool sync;
	struct wl_resource *cached_buffer;
	bool has_cached_buffer;

	// Damage the client reported for the frame being assembled, and what each
	// blob still owes. A blob last written two frames ago is stale wherever
	// either of those frames touched it, so the copy into it has to cover both —
	// buffer age, in the EGL sense.
	struct np_box pending;
	struct np_box owed[2];

	// Enough to rebuild this surface's announcement for a host that attached
	// after the fact. A client tells the compositor its title once, so without a
	// copy the information is gone the moment it is forwarded.
	char *title;
	char *app_id;
	/// Set by xdg_popup.grab. `focus_restore_window` is whatever held focus at
	/// that moment, so a submenu restores its menu and the menu restores the
	/// toplevel without anyone tracking a stack.
	bool has_grab;
	uint32_t focus_restore_window;
	/// Whether a decoration mode has been negotiated, and which one. Only the
	/// zxdg_toplevel_decoration_v1 resource is otherwise kept, and that does not
	/// record the mode it settled on.
	bool decoration_negotiated;
	bool decoration_server_side;
	const char *last_format;
	int32_t last_width, last_height;
	uint32_t last_resource_id;
	uint32_t last_stride;
	const char *last_source;
	bool has_published;

	/// The frame message waiting to go out for this surface.
	///
	/// A commit says "this surface now shows this resource" — a latest value, not
	/// an event with history. Queueing every one lets a busy client outrun the
	/// host and fill the channel, and the overflow then lands on the control
	/// traffic sharing it. Replacing the pending message instead bounds the
	/// backlog by the number of surfaces rather than by time.
	cJSON *pending_frame;

};

static struct np_server *g_server;

/// Set NP_TRACE=1 to see what arrives from the host. Mouse motion is left out
/// because it would bury everything else.
static bool trace_enabled(void) {
	static int enabled = -1;
	if (enabled < 0) enabled = getenv("NP_TRACE") != NULL;
	return enabled == 1;
}


// Commit handling and the subsurface role refer to each other: a parent's commit
// releases its synchronised children, and a released child publishes a frame.
static void flush_sync_children(struct np_surface *parent);
static void flush_pending_frames(struct np_server *server);
static void publish_surface_buffer(struct np_surface *surface, struct wl_resource *buffer);
static void publish_gpu_frame(struct np_surface *surface, struct np_gpu_buffer *gpu);
/// Shared by every per-client input binding, data devices included.
static void input_resource_destroy(struct wl_resource *resource);
// A popup takes keyboard focus when it grabs, which is well before the input
// code that owns focus appears below.
static void set_keyboard_focus(struct np_server *server, uint32_t window_id);
static struct np_surface *surface_by_window(struct np_server *server, uint32_t window_id);

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

static void box_clip(struct np_box *box, int32_t width, int32_t height) {
	if (box->x < 0) { box->width += box->x; box->x = 0; }
	if (box->y < 0) { box->height += box->y; box->y = 0; }
	if (box->x + box->width > width) box->width = width - box->x;
	if (box->y + box->height > height) box->height = height - box->y;
	if (box->width < 0) box->width = 0;
	if (box->height < 0) box->height = 0;
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
	box_union(&surface->pending, x * scale, y * scale, width * scale, height * scale);
}

static void frame_callback_destroy(struct wl_resource *resource) {
	struct np_frame_callback *callback = wl_resource_get_user_data(resource);
	if (!callback) return;
	if (callback->timer) wl_event_source_remove(callback->timer);
	free(callback);
}

static int frame_callback_fire(void *data) {
	struct np_frame_callback *callback = data;
	struct wl_event_source *timer = callback->timer;
	callback->timer = NULL;
	if (timer) wl_event_source_remove(timer);
	wl_callback_send_done(callback->resource, now_ms());
	wl_resource_destroy(callback->resource);
	return 0;
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
	wl_resource_set_implementation(
		callback->resource, NULL, callback, frame_callback_destroy);
	callback->timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(surface->server->display), frame_callback_fire, callback);
	if (!callback->timer) {
		wl_resource_destroy(callback->resource);
		wl_client_post_no_memory(client);
		return;
	}
	// Until the host reports CoreAnimation presentation feedback, use a bounded
	// 60 Hz fallback instead of completing immediately and letting clients spin.
	wl_event_source_timer_update(callback->timer, 16);
}

static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *region) {}
static void surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
                                     struct wl_resource *region) {}

static void surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource,
                                         int32_t transform) {}

static void surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource,
                                     int32_t scale) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->pending_scale = scale > 0 ? scale : 1;
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

/// The one copy. Pixels leave the client's pool and land in host memory.
static void publish_frame(struct np_surface *surface, struct wl_shm_buffer *shm) {
	struct np_server *server = surface->server;

	wl_shm_buffer_begin_access(shm);
	const unsigned char *source = wl_shm_buffer_get_data(shm);
	int32_t width = wl_shm_buffer_get_width(shm);
	int32_t height = wl_shm_buffer_get_height(shm);
	int32_t stride = wl_shm_buffer_get_stride(shm);
	uint32_t format = wl_shm_buffer_get_format(shm);

	// Exactly the size of the frame, because the host displays the whole
	// surface. Padding the allocation and showing a sub-rectangle was tried and
	// reverted; with configures coalesced to the latest size, a reallocation
	// happens once per settled size rather than once per drag step.
	//
	// The row is padded to the host's display alignment, without which
	// CoreAnimation shows nothing at all.
	size_t dst_stride = np_align_row((size_t)stride);
	size_t needed = dst_stride * (size_t)height;
	bool reallocated = false;

	if (surface->blobs[0].data == NULL || surface->blob_stride != (uint32_t)dst_stride ||
	    surface->alloc_height != (uint32_t)height) {
		for (int i = 0; i < 2; i++) np_blob_destroy(server->drm_fd, &surface->blobs[i]);
		for (int i = 0; i < 2; i++) {
			if (!np_blob_create(server->drm_fd, needed, (uint32_t)width, (uint32_t)height,
			                    (uint32_t)dst_stride, &surface->blobs[i])) {
				for (int j = 0; j < 2; j++) np_blob_destroy(server->drm_fd, &surface->blobs[j]);
				surface->blob_stride = 0;
				surface->alloc_width = 0;
				surface->alloc_height = 0;
				wl_shm_buffer_end_access(shm);
				return;
			}
		}
		surface->blob_stride = (uint32_t)dst_stride;
		surface->alloc_width = (uint32_t)width;
		surface->alloc_height = (uint32_t)height;
		surface->back = 0;
		// Both buffers are empty, so neither can be updated incrementally. This
		// is also why a size change must reallocate: reusing a buffer across a
		// resize leaves the newly exposed region holding the previous frame in
		// one buffer and not the other, which alternates as a flicker.
		reallocated = true;
	}

	// What this blob is missing: what changed now, plus what it missed while the
	// other one was on screen.
	struct np_box copy = surface->pending;
	box_union(&copy, surface->owed[surface->back].x, surface->owed[surface->back].y,
	          surface->owed[surface->back].width, surface->owed[surface->back].height);
	if (reallocated || copy.width == 0 || copy.height == 0) {
		copy.x = 0; copy.y = 0; copy.width = width; copy.height = height;
	}
	box_clip(&copy, width, height);

	struct np_blob *blob = &surface->blobs[surface->back];
	unsigned char *destination = blob->data;
	size_t span = (size_t)copy.width * 4;
	size_t offset = (size_t)copy.x * 4;
	for (int32_t row = copy.y; row < copy.y + copy.height; row++) {
		memcpy(destination + (size_t)row * dst_stride + offset,
		       source + (size_t)row * (size_t)stride + offset, span);
	}
	wl_shm_buffer_end_access(shm);

	box_union(&surface->owed[1 - surface->back], surface->pending.x, surface->pending.y,
	          surface->pending.width, surface->pending.height);
	if (reallocated) {
		surface->owed[1 - surface->back].x = 0;
		surface->owed[1 - surface->back].y = 0;
		surface->owed[1 - surface->back].width = width;
		surface->owed[1 - surface->back].height = height;
	}
	box_clear(&surface->owed[surface->back]);
	box_clear(&surface->pending);
	surface->back = 1 - surface->back;

	// ARGB8888 and XRGB8888 are both BGRA byte order on a little-endian guest,
	// which is what CoreAnimation samples.
	const char *format_name = format == WL_SHM_FORMAT_ARGB8888 ? "bgra8888"
		: format == WL_SHM_FORMAT_XRGB8888 ? "bgrx8888"
		: "rgba8888";

	cJSON *frame = cJSON_CreateObject();
	cJSON_AddNumberToObject(frame, "resourceID", blob->resource_id);
	cJSON_AddNumberToObject(frame, "width", width);
	cJSON_AddNumberToObject(frame, "height", height);
	cJSON_AddNumberToObject(frame, "bytesPerRow", (double)dst_stride);
	cJSON_AddStringToObject(frame, "format", format_name);
	cJSON_AddStringToObject(frame, "source", "cpu");
	cJSON_AddNumberToObject(frame, "scale", surface->scale);
	if (surface->geometry_set) {
		cJSON *geometry = cJSON_CreateObject();
		cJSON_AddNumberToObject(geometry, "x", surface->geometry_x);
		cJSON_AddNumberToObject(geometry, "y", surface->geometry_y);
		cJSON_AddNumberToObject(geometry, "width", surface->geometry_width);
		cJSON_AddNumberToObject(geometry, "height", surface->geometry_height);
		cJSON_AddItemToObject(frame, "windowGeometry", geometry);
	}
	cJSON_AddItemToObject(frame, "damage", cJSON_CreateArray());

	surface->last_format = format_name;   // a string literal; nothing to own
	surface->last_width = width;
	surface->last_height = height;
	surface->last_resource_id = blob->resource_id;
	surface->last_stride = (uint32_t)dst_stride;
	surface->last_source = "cpu";
	surface->has_published = true;

	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "surface", surface->id);
	cJSON_AddItemToObject(body, "frame", frame);

	// Supersedes whatever this surface was about to report.
	if (surface->pending_frame) cJSON_Delete(surface->pending_frame);
	surface->pending_frame = body;
}

/// Venus / linux-dmabuf: the resource already is the host GPU image.
/// Name it. Do not copy.
static void publish_gpu_frame(struct np_surface *surface, struct np_gpu_buffer *gpu) {
	if (trace_enabled()) {
		fprintf(stderr, "[wayland] gpu attach surface=%u res=%u %dx%d\n",
		        surface->id, gpu->resource_id, gpu->width, gpu->height);
	}
	const char *format_name = gpu->format == 0x34325241 /* AR24 */ ? "bgra8888"
		: gpu->format == 0x34325258 /* XR24 */ ? "bgrx8888"
		: gpu->format == 0x34324241 /* AB24 */ ? "bgra8888"
		: gpu->format == 0x34324258 /* XB24 */ ? "bgrx8888"
		: "rgba8888";

	cJSON *frame = cJSON_CreateObject();
	cJSON_AddNumberToObject(frame, "resourceID", gpu->resource_id);
	cJSON_AddNumberToObject(frame, "width", gpu->width);
	cJSON_AddNumberToObject(frame, "height", gpu->height);
	cJSON_AddNumberToObject(frame, "bytesPerRow", gpu->stride);
	cJSON_AddStringToObject(frame, "format", format_name);
	cJSON_AddStringToObject(frame, "source", "gpu");
	cJSON_AddNumberToObject(frame, "scale", surface->scale);
	if (surface->geometry_set) {
		cJSON *geometry = cJSON_CreateObject();
		cJSON_AddNumberToObject(geometry, "x", surface->geometry_x);
		cJSON_AddNumberToObject(geometry, "y", surface->geometry_y);
		cJSON_AddNumberToObject(geometry, "width", surface->geometry_width);
		cJSON_AddNumberToObject(geometry, "height", surface->geometry_height);
		cJSON_AddItemToObject(frame, "windowGeometry", geometry);
	}
	cJSON_AddItemToObject(frame, "damage", cJSON_CreateArray());

	surface->last_format = format_name;
	surface->last_width = gpu->width;
	surface->last_height = gpu->height;
	surface->last_resource_id = gpu->resource_id;
	surface->last_stride = (uint32_t)gpu->stride;
	surface->last_source = "gpu";
	surface->has_published = true;

	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "surface", surface->id);
	cJSON_AddItemToObject(body, "frame", frame);
	if (surface->pending_frame) cJSON_Delete(surface->pending_frame);
	surface->pending_frame = body;
}

/// Sends one frame message per surface that has one, at most once per loop.
static void flush_pending_frames(struct np_server *server) {
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (!surface->pending_frame) continue;
		cJSON *body = surface->pending_frame;
		surface->pending_frame = NULL;
		np_host_send(&server->host, "committed", body);
	}
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->scale = surface->pending_scale;
	if (surface->pending_geometry_set) {
		surface->geometry_set = true;
		surface->geometry_x = surface->pending_geometry_x;
		surface->geometry_y = surface->pending_geometry_y;
		surface->geometry_width = surface->pending_geometry_width;
		surface->geometry_height = surface->pending_geometry_height;
		surface->pending_geometry_set = false;
	}

	if (!surface->pending_buffer_set) {
		if (trace_enabled()) {
			fprintf(stderr, "[wayland] commit surface=%u (no buffer)\n", surface->id);
		}
		return;
	}
	struct wl_resource *buffer = surface->pending_buffer;
	surface->pending_buffer = NULL;
	surface->pending_buffer_set = false;
	if (trace_enabled()) {
		fprintf(stderr, "[wayland] commit surface=%u buffer=%s\n",
		        surface->id, buffer ? "set" : "null");
	}

	// A synchronised subsurface's commit does not take effect until its parent
	// commits, so the buffer is held rather than copied.
	if (surface->subsurface && surface->sync) {
		if (surface->has_cached_buffer && surface->cached_buffer) {
			wl_buffer_send_release(surface->cached_buffer);
		}
		surface->cached_buffer = buffer;
		surface->has_cached_buffer = buffer != NULL;
		flush_sync_children(surface);
		return;
	}

	publish_surface_buffer(surface, buffer);
	flush_sync_children(surface);
}

static void publish_surface_buffer(struct np_surface *surface, struct wl_resource *buffer) {
	if (!buffer) return;  // client detached its buffer

	// Attach is where this compositor binds the window to a resource.
	// wl_shm → copy into a host IOSurface. A GPU buffer is already a
	// Venus resource on the host; we only name it. A swapchain present
	// is the next attach of the next image — nothing else to wire.
	struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
	struct np_gpu_buffer *gpu = np_gpu_buffer_get(buffer);
	// Unroled surfaces include cursors and drag icons. A drag icon is commonly
	// committed immediately before start_drag assigns its role, so discarding an
	// unroled commit here makes it impossible for the host to ever show that
	// image. Publish it and let the host retain the latest frame until a role is
	// announced; cursor frames remain harmless cached orphans.
	if (shm) {
		publish_frame(surface, shm);
	} else if (gpu) {
		publish_gpu_frame(surface, gpu);
	} else if (trace_enabled()) {
		fprintf(stderr, "[wayland] commit surface=%u: buffer is neither shm nor gpu\n",
		        surface->id);
	}

	// Released as soon as the copy is done: the client may reuse it, and the
	// host is looking at our blob, not at this buffer.
	wl_buffer_send_release(buffer);
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

static void surface_resource_destroy(struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface) return;
	if (surface->server->drag_icon == resource) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNullToObject(body, "surface");
		np_host_send(&surface->server->host, "dragIconChanged", body);
		surface->server->drag_icon = NULL;
	}

	// Children outlive their parent's resource in principle, and a stale pointer
	// here would later be compared against a reused allocation.
	struct np_surface *child;
	wl_list_for_each(child, &surface->server->surfaces, link) {
		if (child->parent == surface) child->parent = NULL;
	}
	if (surface->toplevel) {
		np_host_send(&surface->server->host, "toplevelDestroyed",
		             object_with_u32("window", surface->window_id));
	}
	if (surface->popup) {
		np_host_send(&surface->server->host, "popupDestroyed",
		             object_with_u32("window", surface->window_id));
	}
	if (surface->subsurface) {
		np_host_send(&surface->server->host, "subsurfaceDestroyed",
		             object_with_u32("surface", surface->id));
	}
	np_host_send(&surface->server->host, "surfaceDestroyed",
	             object_with_u32("surface", surface->id));
	if (surface->pending_frame) {
		cJSON_Delete(surface->pending_frame);
		surface->pending_frame = NULL;
	}
	free(surface->title);
	free(surface->app_id);
	surface->title = surface->app_id = NULL;
	for (int i = 0; i < 2; i++) np_blob_destroy(surface->server->drm_fd, &surface->blobs[i]);
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
	struct np_server *server;
	struct wl_resource *source;
	/// True when the bytes live on the macOS pasteboard, in which case `source`
	/// is NULL and a receive has to make a round trip to the host.
	bool from_host;
	bool dnd;
	uint32_t actions;
	uint32_t preferred_action;
	uint32_t chosen_action;
};

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
	free(wl_resource_get_user_data(resource));
}
static void data_offer_accept(struct wl_client *client, struct wl_resource *resource,
                              uint32_t serial, const char *mime_type) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (offer && offer->source) wl_data_source_send_target(offer->source, mime_type);
}
static void data_offer_finish(struct wl_client *client, struct wl_resource *resource) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (!offer || !offer->dnd || !offer->source) return;
	if (wl_resource_get_version(offer->source) >= 3) {
		wl_data_source_send_dnd_finished(offer->source);
	}
	if (offer->server->drag_source == offer->source) {
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
	if (!offer || !offer->source) return;
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
	state->source = source_resource;
	state->dnd = dnd;
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
		struct wl_resource *offer = wl_resource_create(
			wl_resource_get_client(device), &wl_data_offer_interface,
			wl_resource_get_version(device), 0);
		if (!offer) return;
		struct np_data_offer *state = calloc(1, sizeof(*state));
		if (!state) {
			wl_resource_destroy(offer);
			return;
		}
		state->server = server;
		state->from_host = true;
		wl_resource_set_implementation(offer, &data_offer_implementation, state,
		                               data_offer_resource_destroy);
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
	struct np_surface *surface = surface_by_window(server, window_id);
	if (!surface) return;
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(surface->resource)) continue;
		wl_data_device_send_leave(device->resource);
	}
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
		announce_offer(device->resource, offer, server->drag_source, true);
		wl_data_device_send_enter(device->resource, serial, surface->resource, x, y, offer);
	}
	server->drag_focus_window = surface->window_id;
}

static void drag_send_motion(struct np_server *server, struct np_surface *surface,
                             uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(surface->resource)) continue;
		wl_data_device_send_motion(device->resource, time, x, y);
	}
}

static void drag_finish_on_button_release(struct np_server *server) {
	if (!server->drag_source) return;
	struct np_surface *surface = surface_by_window(server, server->drag_focus_window);
	if (!surface) {
		wl_data_source_send_cancelled(server->drag_source);
		server->drag_source = NULL;
		server->drag_origin = NULL;
		if (server->drag_icon) {
			cJSON *body = cJSON_CreateObject();
			cJSON_AddNullToObject(body, "surface");
			np_host_send(&server->host, "dragIconChanged", body);
		}
		server->drag_icon = NULL;
		server->drag_focus_window = 0;
		return;
	}
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(surface->resource)) continue;
		wl_data_device_send_drop(device->resource);
	}
	if (wl_resource_get_version(server->drag_source) >= 3) {
		wl_data_source_send_dnd_drop_performed(server->drag_source);
	}
	server->drag_dropped = true;
	if (server->drag_icon) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNullToObject(body, "surface");
		np_host_send(&server->host, "dragIconChanged", body);
		server->drag_icon = NULL;
	}
}

static void data_device_start_drag(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *source, struct wl_resource *origin,
                                   struct wl_resource *icon, uint32_t serial) {
	struct np_input *entry = wl_resource_get_user_data(resource);
	if (!entry || !source || !origin) return;
	struct np_server *server = entry->server;
	if (server->drag_source && server->drag_source != source) {
		wl_data_source_send_cancelled(server->drag_source);
	}
	server->drag_source = source;
	server->drag_origin = origin;
	server->drag_icon = icon;
	server->drag_dropped = false;
	cJSON *icon_body = cJSON_CreateObject();
	if (icon) {
		struct np_surface *icon_surface = wl_resource_get_user_data(icon);
		if (icon_surface) cJSON_AddNumberToObject(icon_body, "surface", icon_surface->id);
		else cJSON_AddNullToObject(icon_body, "surface");
	} else {
		cJSON_AddNullToObject(icon_body, "surface");
	}
	np_host_send(&server->host, "dragIconChanged", icon_body);
	struct np_surface *surface = surface_by_window(server, server->pointer_window);
	if (!surface) surface = wl_resource_get_user_data(origin);
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

/// Applies the commits that sync subsurfaces have been holding. Called when the
/// parent commits, which is what "synchronised" means.
static void flush_sync_children(struct np_surface *parent) {
	struct np_surface *child;
	wl_list_for_each(child, &parent->server->surfaces, link) {
		if (child->parent != parent || !child->sync || !child->has_cached_buffer) continue;
		struct wl_resource *buffer = child->cached_buffer;
		child->cached_buffer = NULL;
		child->has_cached_buffer = false;
		publish_surface_buffer(child, buffer);
	}
}

static void subsurface_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void subsurface_set_position(struct wl_client *client, struct wl_resource *resource,
                                    int32_t x, int32_t y) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->sub_x = x;
	surface->sub_y = y;
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "surface", surface->id);
	cJSON_AddNumberToObject(body, "x", x);
	cJSON_AddNumberToObject(body, "y", y);
	np_host_send(&surface->server->host, "subsurfaceMoved", body);
}

static void subsurface_place_above(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *sibling) {
	// Z-order among siblings is CoreAnimation's to keep; the layers are added in
	// creation order and nothing so far reorders them.
}
static void subsurface_place_below(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *sibling) {}

static void subsurface_set_sync(struct wl_client *client, struct wl_resource *resource) {
	((struct np_surface *)wl_resource_get_user_data(resource))->sync = true;
}
static void subsurface_set_desync(struct wl_client *client, struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->sync = false;
	if (surface->has_cached_buffer) {
		struct wl_resource *buffer = surface->cached_buffer;
		surface->cached_buffer = NULL;
		surface->has_cached_buffer = false;
		publish_surface_buffer(surface, buffer);
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
	np_host_send(&surface->server->host, "subsurfaceDestroyed",
	             object_with_u32("surface", surface->id));
	surface->subsurface = NULL;
	surface->parent = NULL;
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

	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "surface", surface->id);
	cJSON_AddNumberToObject(body, "parent", parent->id);
	cJSON_AddNumberToObject(body, "x", surface->sub_x);
	cJSON_AddNumberToObject(body, "y", surface->sub_y);
	np_host_send(&surface->server->host, "subsurfaceCreated", body);
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
	struct np_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (wl_resource_get_client(output->resource) == client) {
			wl_surface_send_enter(surface->resource, output->resource);
		}
	}
	np_host_send(&server->host, "surfaceCreated", object_with_u32("surface", surface->id));
}

static void region_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void region_add(struct wl_client *client, struct wl_resource *resource,
                       int32_t x, int32_t y, int32_t width, int32_t height) {}
static void region_subtract(struct wl_client *client, struct wl_resource *resource,
                            int32_t x, int32_t y, int32_t width, int32_t height) {}

static const struct wl_region_interface region_implementation = {
	.destroy = region_destroy_handler,
	.add = region_add,
	.subtract = region_subtract,
};

static void compositor_create_region(struct wl_client *client, struct wl_resource *resource,
                                     uint32_t id) {
	struct wl_resource *region = wl_resource_create(client, &wl_region_interface, 1, id);
	if (!region) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(region, &region_implementation, NULL, NULL);
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

static void send_size_constraints(struct np_surface *surface, const char *key,
                                  int32_t width, int32_t height) {
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	if (width > 0 && height > 0) {
		cJSON *size = cJSON_CreateObject();
		cJSON_AddNumberToObject(size, "width", width);
		cJSON_AddNumberToObject(size, "height", height);
		cJSON_AddItemToObject(body, key, size);
	} else {
		cJSON_AddNullToObject(body, key);
	}
	np_host_send(&surface->server->host, "sizeConstraintsChanged", body);
}

static void toplevel_set_max_size(struct wl_client *client, struct wl_resource *resource,
                                  int32_t width, int32_t height) {
	send_size_constraints(wl_resource_get_user_data(resource), "maximum", width, height);
}

static void toplevel_set_min_size(struct wl_client *client, struct wl_resource *resource,
                                  int32_t width, int32_t height) {
	send_size_constraints(wl_resource_get_user_data(resource), "minimum", width, height);
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

	// Give the client a concrete initial size. Sending 0x0 means "you choose",
	// but the host also waits for the first frame before creating NSWindow; that
	// leaves some clients trying to allocate a zero-sized Cairo shm surface.
	// The client may still clamp or replace this suggestion before committing.
	struct wl_array states;
	wl_array_init(&states);
	xdg_toplevel_send_configure(surface->toplevel, 800, 600, &states);
	wl_array_release(&states);
	xdg_surface_send_configure(surface->xdg_surface, ++surface->configure_serial);
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
}

static void xdg_surface_ack_configure(struct wl_client *client, struct wl_resource *resource,
                                      uint32_t serial) {}

static void xdg_surface_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
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
	                               surface, NULL);
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
// xdg-decoration
// ---------------------------------------------------------------------------

static void decoration_destroy_handler(struct wl_client *client,
                                       struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void decoration_configure_server_side(struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (surface && surface->toplevel) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "window", surface->window_id);
		cJSON_AddBoolToObject(body, "serverSide", true);
		surface->decoration_negotiated = true;
		surface->decoration_server_side = true;
		np_host_send(&surface->server->host, "decorationModeChanged", body);
	}
	zxdg_toplevel_decoration_v1_send_configure(
		resource, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	// Do not send a second xdg_surface.configure here. vkcube (and Mesa
	// WSI roundtrips on the same wl_display) would dispatch it re-entrantly
	// and Alpine's cube has been observed to SIGSEGV in ack_configure.
}

static void decoration_set_mode(struct wl_client *client, struct wl_resource *resource,
                                uint32_t mode) {
	decoration_configure_server_side(resource);
}

static void decoration_unset_mode(struct wl_client *client, struct wl_resource *resource) {
	decoration_configure_server_side(resource);
}

static const struct zxdg_toplevel_decoration_v1_interface decoration_implementation = {
	.destroy = decoration_destroy_handler,
	.set_mode = decoration_set_mode,
	.unset_mode = decoration_unset_mode,
};

static void decoration_resource_destroy(struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (surface && surface->decoration == resource) surface->decoration = NULL;
}

static void decoration_manager_destroy_handler(struct wl_client *client,
                                               struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void decoration_manager_get_toplevel_decoration(
	struct wl_client *client, struct wl_resource *manager, uint32_t id,
	struct wl_resource *toplevel_resource) {
	struct np_surface *surface = wl_resource_get_user_data(toplevel_resource);
	if (!surface || surface->decoration) {
		wl_resource_post_error(manager,
			ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
			"toplevel already has a decoration object");
		return;
	}
	struct wl_resource *decoration = wl_resource_create(
		client, &zxdg_toplevel_decoration_v1_interface,
		wl_resource_get_version(manager), id);
	if (!decoration) {
		wl_client_post_no_memory(client);
		return;
	}
	surface->decoration = decoration;
	wl_resource_set_implementation(decoration, &decoration_implementation, surface,
	                               decoration_resource_destroy);
	decoration_configure_server_side(decoration);
}

static const struct zxdg_decoration_manager_v1_interface decoration_manager_implementation = {
	.destroy = decoration_manager_destroy_handler,
	.get_toplevel_decoration = decoration_manager_get_toplevel_decoration,
};

static void decoration_manager_bind(struct wl_client *client, void *data,
                                    uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &zxdg_decoration_manager_v1_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(
		resource, &decoration_manager_implementation, data, NULL);
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

// ---------------------------------------------------------------------------
// wp_fractional_scale_v1
// ---------------------------------------------------------------------------

/// The protocol counts in 120ths, so 120 is 1x and 240 is 2x.
#define NP_SCALE_UNIT 120

static void send_preferred_scale(struct np_surface *surface, int scale) {
	if (!surface->fractional_scale || scale <= 0) return;
	if (surface->reported_scale == scale) return;
	surface->reported_scale = scale;
	wp_fractional_scale_v1_send_preferred_scale(surface->fractional_scale,
	                                            (uint32_t)(scale * NP_SCALE_UNIT));
}

static void fractional_scale_destroy_handler(struct wl_client *client,
                                             struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_fractional_scale_v1_interface fractional_scale_implementation = {
	.destroy = fractional_scale_destroy_handler,
};

static void fractional_scale_resource_destroy(struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (surface) surface->fractional_scale = NULL;
}

static void fractional_manager_destroy_handler(struct wl_client *client,
                                               struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void fractional_manager_get_scale(struct wl_client *client, struct wl_resource *resource,
                                         uint32_t id, struct wl_resource *surface_resource) {
	struct np_surface *surface = wl_resource_get_user_data(surface_resource);
	struct wl_resource *scale = wl_resource_create(
		client, &wp_fractional_scale_v1_interface, wl_resource_get_version(resource), id);
	if (!scale) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(scale, &fractional_scale_implementation, surface,
	                               fractional_scale_resource_destroy);
	surface->fractional_scale = scale;
	surface->reported_scale = 0;
	send_preferred_scale(surface, surface->server->output_scale);
}

static const struct wp_fractional_scale_manager_v1_interface fractional_manager_implementation = {
	.destroy = fractional_manager_destroy_handler,
	.get_fractional_scale = fractional_manager_get_scale,
};

static void fractional_manager_bind(struct wl_client *client, void *data,
                                    uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &wp_fractional_scale_manager_v1_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &fractional_manager_implementation, data, NULL);
}

/// One output standing in for the macOS display the window is on. It never
/// scans out; it exists so clients can learn the scale to render at.
static void output_resource_destroy(struct wl_resource *resource) {
	struct np_output *output = wl_resource_get_user_data(resource);
	if (!output) return;
	wl_list_remove(&output->link);
	free(output);
}

static void output_send_state(struct np_server *server, struct wl_resource *resource) {
	uint32_t version = (uint32_t)wl_resource_get_version(resource);
	if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) {
		wl_output_send_scale(resource, server->output_scale);
	}
	if (version >= WL_OUTPUT_DONE_SINCE_VERSION) wl_output_send_done(resource);
}

static void output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct np_server *server = data;
	struct wl_resource *resource =
		wl_resource_create(client, &wl_output_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_output *output = calloc(1, sizeof(*output));
	if (output) {
		output->resource = resource;
		wl_list_insert(&server->outputs, &output->link);
		wl_resource_set_implementation(resource, NULL, output, output_resource_destroy);
	}
	// A zero physical size is technically usable as "unknown", but it leaves
	// DPI calculation up to client-toolkit fallbacks.  GTK/Pango on the minimal
	// Alpine image turns that into pathological widget requisitions (millions of
	// pixels high).  Describe the built-in display consistently with the mode:
	// 3024x1964 at roughly 220 dpi.
	wl_output_send_geometry(resource, 0, 0, 345, 224, WL_OUTPUT_SUBPIXEL_UNKNOWN,
	                        "Apple", "NativePipe", WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
	                    server->output_width, server->output_height, 60000);
	output_send_state(server, resource);

	// Also cover clients that created a surface before binding wl_output.
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (wl_resource_get_client(surface->resource) == client) {
			wl_surface_send_enter(surface->resource, resource);
		}
	}
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
	struct np_surface *surface = surface_by_window(server, window_id);
	if (!surface) return;
	bool entering = server->pointer_window != window_id;
	server->pointer_x = wl_fixed_to_double(x);
	server->pointer_y = wl_fixed_to_double(y);

	struct np_input *entry;
	uint32_t serial = wl_display_next_serial(server->display);
	uint32_t time = now_ms();
	if (server->drag_source) {
		if (server->drag_focus_window != window_id) {
			if (server->drag_focus_window) drag_send_leave(server, server->drag_focus_window);
			drag_send_enter(server, surface, x, y);
		} else {
			drag_send_motion(server, surface, time, x, y);
		}
		wl_display_flush_clients(server->display);
		return;
	}

	// Tracking areas belonging to two NSWindows can overlap briefly while a
	// child window is ordered. Repair that host ordering to Wayland's single
	// pointer-focus, leave-before-enter model.
	if (entering && server->pointer_window) {
		struct np_surface *previous = surface_by_window(server, server->pointer_window);
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
		if (entering) wl_pointer_send_enter(entry->resource, serial, surface->resource, x, y);
		else wl_pointer_send_motion(entry->resource, time, x, y);
		pointer_frame(entry->resource);
	}
	server->pointer_window = window_id;
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
	struct np_surface *surface = surface_by_window(server, window_id);
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

/// Binary fast path for the only high-rate replaceable command. Its payload is
/// "NPMO", window u32, x i32 24.8 and y i32 24.8, all little endian.
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

	if (strcmp(name, "configure") == 0) {
		uint32_t window_id = (uint32_t)json_int(body, "window", 0);
		struct np_surface *surface = surface_by_window(server, window_id);
		if (!surface || !surface->toplevel) return;

		cJSON *size = cJSON_GetObjectItemCaseSensitive(body, "size");
		int width = size ? json_int(size, "width", 0) : 0;
		int height = size ? json_int(size, "height", 0) : 0;

		// Geometry reaches the client in surface-local pixels divided by the
		// buffer scale, which is what xdg_toplevel.configure is defined in.
		struct wl_array states;
		wl_array_init(&states);
		cJSON *state_list = cJSON_GetObjectItemCaseSensitive(body, "states");
		cJSON *state;
		cJSON_ArrayForEach(state, state_list) {
			if (!cJSON_IsString(state)) continue;
			uint32_t value = 0;
			if (strcmp(state->valuestring, "maximized") == 0) value = XDG_TOPLEVEL_STATE_MAXIMIZED;
			else if (strcmp(state->valuestring, "fullscreen") == 0) value = XDG_TOPLEVEL_STATE_FULLSCREEN;
			else if (strcmp(state->valuestring, "resizing") == 0) value = XDG_TOPLEVEL_STATE_RESIZING;
			else if (strcmp(state->valuestring, "activated") == 0) value = XDG_TOPLEVEL_STATE_ACTIVATED;
			if (!value) continue;
			uint32_t *slot = wl_array_add(&states, sizeof(uint32_t));
			if (slot) *slot = value;
		}
		xdg_toplevel_send_configure(surface->toplevel, width / surface->scale,
		                            height / surface->scale, &states);
		wl_array_release(&states);
		xdg_surface_send_configure(surface->xdg_surface, ++surface->configure_serial);
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
		if (server->drag_source && server->drag_focus_window == window_id) {
			drag_send_leave(server, window_id);
			server->drag_focus_window = 0;
		}
		// Ignore a delayed exit from the window that lost focus during the
		// leave-before-enter repair above.
		if (server->pointer_window != window_id) return;
		struct np_surface *surface = surface_by_window(server, window_id);
		if (!surface) return;
		struct np_input *entry;
		uint32_t serial = wl_display_next_serial(server->display);
		wl_list_for_each(entry, &server->pointers, link) {
			if (!same_client(entry->resource, surface->resource)) continue;
			wl_pointer_send_leave(entry->resource, serial, surface->resource);
			pointer_frame(entry->resource);
		}
		server->pointer_window = 0;
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "pointerButton") == 0) {
		struct np_surface *surface = surface_by_window(server, (uint32_t)json_int(body, "window", 0));
		if (!surface) return;
		cJSON *which = cJSON_GetObjectItemCaseSensitive(body, "button");
		uint32_t code = BTN_LEFT;
		if (cJSON_IsString(which)) {
			if (strcmp(which->valuestring, "right") == 0) code = BTN_RIGHT;
			else if (strcmp(which->valuestring, "middle") == 0) code = BTN_MIDDLE;
		}
		bool pressed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "pressed"));
		if (!pressed && server->drag_source) {
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
		if (surface) send_preferred_scale(surface, scale);

		// wl_output still carries a scale for clients too old to know about
		// wp_fractional_scale_v1; the primary display's is the best single
		// answer available.
		if (scale != server->output_scale) {
			server->output_scale = scale;
			struct np_output *output;
			wl_list_for_each(output, &server->outputs, link) {
				output_send_state(server, output->resource);
			}
		}
		wl_display_flush_clients(server->display);
		return;
	}
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void sync_host_connection_source(struct np_server *server);

static int host_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&server->host);
	np_host_pump(&server->host, handle_host_command, handle_host_binary, server);
	sync_host_connection_source(server);
	flush_pending_frames(server);
	wl_display_flush_clients(server->display);
	return 0;
}

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
		} else if (surface->subsurface && surface->parent) {
			body = cJSON_CreateObject();
			cJSON_AddNumberToObject(body, "surface", surface->id);
			cJSON_AddNumberToObject(body, "parent", surface->parent->id);
			cJSON_AddNumberToObject(body, "x", surface->sub_x);
			cJSON_AddNumberToObject(body, "y", surface->sub_y);
			np_host_send(&server->host, "subsurfaceCreated", body);
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

	// Finally the pixels. The host creates an NSWindow only once a frame has
	// arrived, so replaying the announcements alone would leave it with a window
	// list and nothing on screen. The last published blob still holds the
	// contents — the front buffer, since `back` already flipped past it.
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		if (!surface->has_published) continue;
		uint32_t resource_id = surface->last_resource_id;
		if (!resource_id) {
			struct np_blob *blob = &surface->blobs[1 - surface->back];
			resource_id = blob->resource_id;
		}
		if (!resource_id) continue;

		cJSON *frame = cJSON_CreateObject();
		cJSON_AddNumberToObject(frame, "resourceID", resource_id);
		cJSON_AddNumberToObject(frame, "width", surface->last_width);
		cJSON_AddNumberToObject(frame, "height", surface->last_height);
		cJSON_AddNumberToObject(frame, "bytesPerRow",
		                        (double)(surface->last_stride ? surface->last_stride
		                                                     : surface->blob_stride));
		cJSON_AddStringToObject(frame, "format", surface->last_format);
		if (surface->last_source) {
			cJSON_AddStringToObject(frame, "source", surface->last_source);
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
		cJSON_AddItemToObject(frame, "damage", cJSON_CreateArray());

		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "surface", surface->id);
		cJSON_AddItemToObject(body, "frame", frame);

		// Straight into the pending slot, so a commit racing this replay wins.
		if (surface->pending_frame) cJSON_Delete(surface->pending_frame);
		surface->pending_frame = body;
	}
}

static void sync_host_connection_source(struct np_server *server) {
	// Detected before the mask is computed, so the replay it queues is what
	// arms the writability watch below.
	if (server->watched_host_fd < 0 && server->host.conn_fd >= 0)
		republish_state(server);

	// Writability is watched only while something is waiting to go out. Without
	// it, a socket that returned EAGAIN leaves the backlog parked until some
	// unrelated fd happens to wake the loop — and with an idle client and a
	// quiet host, nothing ever does.
	uint32_t mask = WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR;
	if (np_host_has_backlog(&server->host)) mask |= WL_EVENT_WRITABLE;

	if (server->watched_host_fd == server->host.conn_fd) {
		if (server->host_connection_source && server->watched_host_mask != mask) {
			wl_event_source_fd_update(server->host_connection_source, mask);
			server->watched_host_mask = mask;
		}
		return;
	}

	if (server->host_connection_source) {
		wl_event_source_remove(server->host_connection_source);
		server->host_connection_source = NULL;
	}
	server->watched_host_fd = server->host.conn_fd;
	server->watched_host_mask = mask;

	if (server->host.conn_fd >= 0) {
		struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
		server->host_connection_source = wl_event_loop_add_fd(
			loop, server->host.conn_fd, mask, host_channel_readable, server);
	}
}

static int host_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	np_host_pump(&server->host, handle_host_command, handle_host_binary, server);
	sync_host_connection_source(server);
	flush_pending_frames(server);
	wl_display_flush_clients(server->display);
	return 0;
}

int main(int argc, char **argv) {
	struct np_server server;
	memset(&server, 0, sizeof(server));
	server.next_id = 1;
	server.output_scale = 2;
	server.output_width = 3024;
	server.output_height = 1964;
	wl_list_init(&server.surfaces);
	wl_list_init(&server.outputs);
	wl_list_init(&server.pointers);
	wl_list_init(&server.keyboards);
	wl_list_init(&server.data_devices);
	wl_list_init(&server.clip_reads);
	wl_list_init(&server.clip_writes);
	wl_list_init(&server.text_inputs);
	// Clipboard transfers write into pipes owned by other processes, and a
	// client that closes its end mid-paste would otherwise take the compositor
	// down with it.
	signal(SIGPIPE, SIG_IGN);
	server.keymap_fd = -1;
	server.watched_host_fd = -1;
	g_server = &server;

	server.drm_fd = np_blob_open();
	if (server.drm_fd < 0) {
		fprintf(stderr, "[wayland] no virtio-gpu render node; cannot allocate host buffers\n");
		return 1;
	}

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
	wl_global_create(server.display, &wl_output_interface, OUTPUT_VERSION, &server, output_bind);
	// Do not advertise fractional-scale until wp_viewporter is implemented.
	// The protocols are a pair: GTK and Qt create a fractional-scale object only
	// when they can apply its value through a viewport. Advertising half of the
	// contract produced multi-million-pixel shm buffers and client crashes.
	wl_global_create(server.display, &zwp_text_input_manager_v3_interface, 1,
	                 &server, text_input_manager_bind);
	wl_global_create(server.display, &zxdg_decoration_manager_v1_interface, 1,
	                 &server, decoration_manager_bind);
	np_dmabuf_advertise(server.display, server.drm_fd);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (!socket) {
		fprintf(stderr, "[wayland] could not create a Wayland socket\n");
		return 1;
	}
	fprintf(stderr, "[wayland] WAYLAND_DISPLAY=%s\n", socket);

	if (!create_keymap(&server)) {
		fprintf(stderr, "[wayland] no keymap; keyboard input will not work\n");
	}

	if (!np_host_listen(&server.host)) return 1;

	// The host channel is a file descriptor like any other, so it belongs in the
	// same event loop as the Wayland clients. Polling it after each dispatch made
	// the two starve each other: a slow Wayland turn delayed every host command,
	// and an idle loop still woke up sixty times a second.
	struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
	wl_event_loop_add_fd(loop, server.host.listen_fd, WL_EVENT_READABLE,
	                     host_listener_readable, &server);
	for (;;) {
		flush_pending_frames(&server);
		wl_display_flush_clients(server.display);
		wl_event_loop_dispatch(loop, -1);
		np_host_pump(&server.host, handle_host_command, handle_host_binary, &server);
		sync_host_connection_source(&server);
		flush_pending_frames(&server);
	}

	np_host_finish(&server.host);
	wl_display_destroy(server.display);
	np_blob_close(server.drm_fd);
	return 0;
}
