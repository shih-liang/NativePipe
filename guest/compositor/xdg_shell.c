#include "xdg_shell.h"

#include "compositor_internal.h"
#include "decoration.h"
#include "hostlink.h"
#include "window_events.h"
#include "windowwire.h"
#include "xdg-shell-server-protocol.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

static cJSON *object_with_u32(const char *key, uint32_t value) {
	cJSON *object = cJSON_CreateObject();
	cJSON_AddNumberToObject(object, key, value);
	return object;
}

static void schedule_pending_host_toplevel_configure(struct np_surface *surface);

static bool valid_pointer_grab(struct np_surface *surface,
	                           struct wl_client *client, uint32_t serial) {
	return surface && surface->server->pointer_button_down &&
	       surface->server->pointer_grab_client == client &&
	       surface->server->pointer_grab_serial == serial;
}

static bool popup_has_child(struct np_surface *surface) {
	struct np_surface *candidate;
	wl_list_for_each(candidate, &surface->server->surfaces, link) {
		if (candidate->popup &&
		    candidate->popup_parent_window == surface->window_id)
			return true;
	}
	return false;
}

static void post_wm_error(struct np_surface *surface, uint32_t code,
	                      const char *message) {
	struct wl_resource *resource = surface && surface->xdg_wm_base
		? surface->xdg_wm_base : surface ? surface->xdg_surface : NULL;
	if (resource) wl_resource_post_error(resource, code, "%s", message);
}

void np_xdg_clear_configures(struct np_surface *surface) {
	struct np_xdg_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &surface->xdg_configures, link) {
		wl_list_remove(&configure->link);
		free(configure);
	}
	surface->latest_configure_serial = 0;
	surface->host_configure_acked_serial = 0;
	surface->host_configure_acked = false;
}

static uint32_t send_xdg_surface_configure(struct np_surface *surface) {
	if (!surface || !surface->xdg_surface) return 0;
	struct np_xdg_configure *configure = calloc(1, sizeof(*configure));
	if (!configure) {
		wl_client_post_no_memory(wl_resource_get_client(surface->xdg_surface));
		return 0;
	}
	configure->serial = wl_display_next_serial(surface->server->display);
	wl_list_insert(surface->xdg_configures.prev, &configure->link);
	surface->latest_configure_serial = configure->serial;
	xdg_surface_send_configure(surface->xdg_surface, configure->serial);
	return configure->serial;
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
	if (!valid_pointer_grab(surface, client, serial)) return;
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	cJSON_AddNumberToObject(body, "serial", serial);
	np_host_send(&surface->server->host, "interactiveMoveRequested", body);
}

static void toplevel_resize(struct wl_client *client, struct wl_resource *resource,
                            struct wl_resource *seat, uint32_t serial, uint32_t edges) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!valid_pointer_grab(surface, client, serial) ||
	    edges == XDG_TOPLEVEL_RESIZE_EDGE_NONE ||
	    !xdg_toplevel_resize_edge_is_valid(
		    edges, wl_resource_get_version(resource)))
		return;
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
	uint32_t fields[] = {surface->window_id};
	np_window_event_send(surface->server, NP_GUEST_TOPLEVEL_DESTROYED, fields, 1);
	surface->toplevel = NULL;
	surface->xdg_configure_phase = NP_XDG_NO_ROLE;
	np_xdg_clear_configures(surface);
	surface->mapped = false;
}

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
	uint32_t serial = send_xdg_surface_configure(surface);
	if (np_trace_enabled())
		fprintf(stderr,
		        "[configure] send surface=%u serial=%u size=%dx%d states=0x%x\n",
		        surface->id, serial, width, height, state_bits);
}

void np_xdg_send_initial_role_configure(struct np_surface *surface) {
	if (!surface || !surface->xdg_surface) return;
	if (surface->toplevel) {
		send_host_toplevel_configure(surface, 800, 600, 0);
	} else if (surface->popup) {
		/* The bufferless initial commit must always produce the popup's first
		 * configure.  Waiting for a host round-trip leaves GTK/Qt/Firefox with
		 * an unconfigured role and lets content race ahead at undefined geometry.
		 * Screen-edge adjustment remains a later reposition operation. */
		np_xdg_configure_popup(
			surface, surface->popup_x, surface->popup_y,
			surface->popup_width, surface->popup_height, 0);
	}
}

void np_xdg_configure_popup(struct np_surface *surface,
	                        int32_t x, int32_t y,
	                        int32_t width, int32_t height,
	                        uint32_t token) {
	if (!surface || !surface->popup || !surface->xdg_surface ||
	    width <= 0 || height <= 0) return;
	/* A response to an older skipped reposition must not move a newer menu. */
	if (token && token != surface->popup_requested_token) return;
	if (token && wl_resource_get_version(surface->popup) >=
	             XDG_POPUP_REPOSITIONED_SINCE_VERSION)
		xdg_popup_send_repositioned(surface->popup, token);
	xdg_popup_send_configure(surface->popup, x, y, width, height);
	uint32_t serial = send_xdg_surface_configure(surface);
	if (!serial) return;
	struct np_xdg_configure *configure = wl_container_of(
		surface->xdg_configures.prev, configure, link);
	configure->popup_geometry = true;
	configure->popup_x = x;
	configure->popup_y = y;
	configure->popup_width = width;
	configure->popup_height = height;
}

void np_xdg_apply_popup_geometry(struct np_surface *surface,
	                             int32_t x, int32_t y,
	                             int32_t width, int32_t height) {
	if (!surface || !surface->popup) return;
	surface->popup_x = x;
	surface->popup_y = y;
	surface->popup_width = width;
	surface->popup_height = height;
	uint32_t fields[] = {
		surface->window_id, (uint32_t)x, (uint32_t)y,
		(uint32_t)width, (uint32_t)height,
	};
	np_window_event_send(surface->server, NP_GUEST_POPUP_REPOSITIONED,
	                     fields, 5);
}

void np_xdg_queue_toplevel_configure(struct np_surface *surface,
	                                      int32_t width, int32_t height,
	                                      uint32_t state_bits) {
	/* The host transport pump drains every currently available record before
	 * returning to Wayland. Keep only its newest size, then emit one configure
	 * from the following idle. Do not wait for an older configure's ack/commit:
	 * xdg-shell explicitly lets clients discard every configure but the latest.
	 * Whether a Vulkan client rebuilds for every delivered extent is its WSI
	 * policy; the compositor must not add an ack/commit dependency of its own. */
	surface->host_configure_pending = true;
	surface->host_configure_pending_width = width;
	surface->host_configure_pending_height = height;
	surface->host_configure_pending_state_bits = state_bits;
	schedule_pending_host_toplevel_configure(surface);
}

static void send_pending_host_toplevel_configure(struct np_surface *surface) {
	if (!surface || !surface->host_configure_pending)
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

void np_xdg_finish_toplevel_configure(struct np_surface *surface,
	                                       uint32_t serial) {
	if (!surface || !serial) return;
	if (np_trace_enabled())
		fprintf(stderr,
		        "[configure] committed surface=%u acked=%u current=%u serial=%u pending=%d\n",
		        surface->id, surface->host_configure_acked_serial,
		        surface->latest_configure_serial, serial,
		        surface->host_configure_pending);
	if (surface->host_configure_acked_serial == serial) {
		surface->host_configure_acked_serial = 0;
		surface->host_configure_acked = false;
	}
}

static void xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *resource,
                                     uint32_t id) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface || surface->toplevel || surface->popup) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
		                       "xdg_surface already has a role object");
		return;
	}
	if (!np_surface_assign_role(surface, NP_SURFACE_ROLE_XDG_TOPLEVEL)) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
		                       "wl_surface has another permanent role");
		return;
	}
	surface->toplevel = wl_resource_create(
		client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
	if (!surface->toplevel) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(surface->toplevel, &toplevel_implementation, surface,
	                               toplevel_resource_destroy);
	surface->window_id = surface->server->next_id++;
	surface->xdg_configure_phase = NP_XDG_AWAITING_INITIAL_COMMIT;
	surface->mapped = false;

	uint32_t fields[] = {surface->window_id, surface->id};
	np_window_event_send(surface->server, NP_GUEST_TOPLEVEL_CREATED, fields, 2);
	/* Decoration policy is compositor state.  Send the client-side default
	 * before the first frame so AppKit never constructs a transient second
	 * titlebar around a CSD window. */
	np_decoration_use_client_default(surface);

	/* The initial 800x600 suggestion is sent only after the role's required
	 * bufferless wl_surface.commit. Sending it here races the client role setup
	 * and violates xdg-shell's initial configure handshake. */
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
	uint32_t constraint_adjustment;
	bool reactive;
	bool parent_size_set;
	int32_t parent_width, parent_height;
	bool parent_configure_set;
	uint32_t parent_configure;
};

static void positioner_resolve_values(const struct np_positioner *p,
	                                  uint32_t anchor, uint32_t gravity,
	                                  int32_t *out_x, int32_t *out_y) {
	// The anchor picks a point on the rectangle...
	int32_t x = p->anchor_x, y = p->anchor_y;
	switch (anchor) {
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
	switch (gravity) {
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

static uint32_t flip_x(uint32_t value) {
	switch (value) {
	case XDG_POSITIONER_ANCHOR_LEFT: return XDG_POSITIONER_ANCHOR_RIGHT;
	case XDG_POSITIONER_ANCHOR_RIGHT: return XDG_POSITIONER_ANCHOR_LEFT;
	case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
	default: return value;
	}
}

static uint32_t flip_y(uint32_t value) {
	switch (value) {
	case XDG_POSITIONER_ANCHOR_TOP: return XDG_POSITIONER_ANCHOR_BOTTOM;
	case XDG_POSITIONER_ANCHOR_BOTTOM: return XDG_POSITIONER_ANCHOR_TOP;
	case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
	default: return value;
	}
}

static void positioner_resolve(const struct np_positioner *p,
	                           int32_t *x, int32_t *y,
	                           int32_t *flipped_x, int32_t *flipped_y) {
	positioner_resolve_values(p, p->anchor, p->gravity, x, y);
	int32_t ignored;
	positioner_resolve_values(
		p, flip_x(p->anchor), flip_x(p->gravity), flipped_x, &ignored);
	positioner_resolve_values(
		p, flip_y(p->anchor), flip_y(p->gravity), &ignored, flipped_y);
}

static void positioner_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void positioner_set_size(struct wl_client *client, struct wl_resource *resource,
                                int32_t width, int32_t height) {
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
		                       "positioner size must be positive");
		return;
	}
	struct np_positioner *p = wl_resource_get_user_data(resource);
	p->width = width; p->height = height;
}
static void positioner_set_anchor_rect(struct wl_client *client, struct wl_resource *resource,
                                       int32_t x, int32_t y, int32_t width, int32_t height) {
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
		                       "anchor rectangle must be positive");
		return;
	}
	struct np_positioner *p = wl_resource_get_user_data(resource);
	p->anchor_x = x; p->anchor_y = y; p->anchor_width = width; p->anchor_height = height;
}
static void positioner_set_anchor(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t anchor) {
	if (!xdg_positioner_anchor_is_valid(anchor, wl_resource_get_version(resource))) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
		                       "invalid anchor");
		return;
	}
	((struct np_positioner *)wl_resource_get_user_data(resource))->anchor = anchor;
}
static void positioner_set_gravity(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t gravity) {
	if (!xdg_positioner_gravity_is_valid(gravity, wl_resource_get_version(resource))) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
		                       "invalid gravity");
		return;
	}
	((struct np_positioner *)wl_resource_get_user_data(resource))->gravity = gravity;
}
static void positioner_set_constraint_adjustment(struct wl_client *client,
                                                 struct wl_resource *resource,
                                                 uint32_t adjustment) {
	if (!xdg_positioner_constraint_adjustment_is_valid(
			adjustment, wl_resource_get_version(resource))) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
		                       "invalid constraint adjustment");
		return;
	}
	((struct np_positioner *)wl_resource_get_user_data(resource))
		->constraint_adjustment = adjustment;
}
static void positioner_set_offset(struct wl_client *client, struct wl_resource *resource,
                                  int32_t x, int32_t y) {
	struct np_positioner *p = wl_resource_get_user_data(resource);
	p->offset_x = x; p->offset_y = y;
}
static void positioner_set_reactive(struct wl_client *client, struct wl_resource *resource) {
	((struct np_positioner *)wl_resource_get_user_data(resource))->reactive = true;
}
static void positioner_set_parent_size(struct wl_client *client, struct wl_resource *resource,
                                       int32_t width, int32_t height) {
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
		                       "parent size must be positive");
		return;
	}
	struct np_positioner *p = wl_resource_get_user_data(resource);
	p->parent_size_set = true;
	p->parent_width = width;
	p->parent_height = height;
}
static void positioner_set_parent_configure(struct wl_client *client, struct wl_resource *resource,
                                            uint32_t serial) {
	struct np_positioner *p = wl_resource_get_user_data(resource);
	p->parent_configure_set = true;
	p->parent_configure = serial;
}

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
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (surface && popup_has_child(surface)) {
		post_wm_error(surface, XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
		              "destroy nested popups from topmost to parent");
		return;
	}
	wl_resource_destroy(resource);
}

static void popup_grab(struct wl_client *client, struct wl_resource *resource,
                       struct wl_resource *seat, uint32_t serial) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface) return;
	struct np_server *server = surface->server;
	struct np_surface *parent = np_surface_by_window(
		server, surface->popup_parent_window);
	if (!parent || (parent->popup && !parent->has_grab)) {
		post_wm_error(surface, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
		              "grabbing popup parent is not a toplevel or grabbing popup");
		return;
	}
	if (surface->has_grab) return;
	if (server->last_input_client != client ||
	    server->last_input_serial != serial) {
		xdg_popup_send_popup_done(resource);
		return;
	}

	// The grab is what makes a menu dismiss when the user clicks elsewhere. The
	// host decides when that has happened and sends dismissPopup back.
	//
	// Keyboard focus, though, has to move here rather than follow the host. A
	// popup is a non-activating NSPanel, so it never becomes key and macOS goes
	// on reporting the parent as focused. Leaving focus there would send the
	// menu's arrow keys and mnemonics to the surface underneath it.
	surface->has_grab = true;
	surface->focus_restore_window = server->focused_window;
	np_set_keyboard_focus(server, surface->window_id);
	wl_display_flush_clients(server->display);
}

static bool popup_use_positioner(struct np_surface *surface,
	                             const struct np_positioner *p,
	                             uint32_t token) {
	if (!surface || !p || p->width <= 0 || p->height <= 0 ||
	    p->anchor_width <= 0 || p->anchor_height <= 0)
		return false;
	positioner_resolve(p, &surface->popup_x, &surface->popup_y,
	                   &surface->popup_flip_x, &surface->popup_flip_y);
	surface->popup_width = p->width;
	surface->popup_height = p->height;
	surface->popup_constraint_adjustment = p->constraint_adjustment;
	surface->popup_reactive = p->reactive;
	surface->popup_requested_token = token;
	return true;
}

static void popup_reposition(struct wl_client *client, struct wl_resource *resource,
                             struct wl_resource *positioner, uint32_t token) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	struct np_positioner *p = wl_resource_get_user_data(positioner);
	if (!popup_use_positioner(surface, p, token)) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
		                       "incomplete popup positioner");
		return;
	}
	np_window_event_send_popup_placement(
		surface->server, surface->window_id, surface->popup_parent_window,
		surface->popup_x, surface->popup_y,
		surface->popup_flip_x, surface->popup_flip_y,
		surface->popup_width, surface->popup_height,
		surface->popup_constraint_adjustment, token, surface->popup_reactive);
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
			np_set_keyboard_focus(server,
			                   np_surface_by_window(server, restore) ? restore : 0);
		}
	}
	uint32_t fields[] = {surface->window_id};
	np_window_event_send(surface->server, NP_GUEST_POPUP_DESTROYED, fields, 1);
	surface->popup = NULL;
	surface->xdg_configure_phase = NP_XDG_NO_ROLE;
	np_xdg_clear_configures(surface);
	surface->mapped = false;
}

static void xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id, struct wl_resource *parent,
                                  struct wl_resource *positioner) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	struct np_surface *parent_surface = parent ? wl_resource_get_user_data(parent) : NULL;
	struct np_positioner *p = wl_resource_get_user_data(positioner);
	if (!surface || surface->toplevel || surface->popup) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
		                       "xdg_surface already has a role object");
		return;
	}
	if (!np_surface_assign_role(surface, NP_SURFACE_ROLE_XDG_POPUP)) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
		                       "wl_surface has another permanent role");
		return;
	}
	if (!parent_surface || (!parent_surface->toplevel && !parent_surface->popup) ||
	    wl_resource_get_client(parent_surface->resource) != client) {
		post_wm_error(surface, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
		              "popup parent is not a constructed xdg surface");
		return;
	}

	surface->popup = wl_resource_create(
		client, &xdg_popup_interface, wl_resource_get_version(resource), id);
	if (!surface->popup) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(surface->popup, &popup_implementation, surface,
	                               popup_resource_destroy);
	surface->window_id = surface->server->next_id++;
	surface->xdg_configure_phase = NP_XDG_AWAITING_INITIAL_COMMIT;
	surface->mapped = false;

	if (!popup_use_positioner(surface, p, 0)) {
		wl_resource_destroy(surface->popup);
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
		                       "incomplete popup positioner");
		return;
	}
	surface->popup_parent_window = parent_surface ? parent_surface->window_id : 0;
	uint32_t fields[] = {
		surface->window_id, surface->id, surface->popup_parent_window,
		(uint32_t)surface->popup_x, (uint32_t)surface->popup_y,
		(uint32_t)surface->popup_width, (uint32_t)surface->popup_height,
	};
	np_window_event_send(surface->server, NP_GUEST_POPUP_CREATED, fields, 7);
}

static void xdg_surface_set_window_geometry(struct wl_client *client, struct wl_resource *resource,
	                                        int32_t x, int32_t y, int32_t width, int32_t height) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface || (!surface->toplevel && !surface->popup)) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
		                       "xdg_surface has no role object");
		return;
	}
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
	if (np_trace_enabled())
		fprintf(stderr, "[wayland] window geometry surface=%u %d,%d %dx%d\n",
		        surface->id, x, y, width, height);
}

static void xdg_surface_ack_configure(struct wl_client *client, struct wl_resource *resource,
	                                  uint32_t serial) {
	(void)client;
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface) return;
	if (!surface->toplevel && !surface->popup) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
		                       "xdg_surface has no role object");
		return;
	}
	struct np_xdg_configure *configure, *matched = NULL;
	wl_list_for_each(configure, &surface->xdg_configures, link) {
		if (configure->serial == serial) {
			matched = configure;
			break;
		}
	}
	if (!matched) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
		                       "invalid configure serial %u", serial);
		return;
	}
	if (matched->popup_geometry) {
		surface->popup_geometry_acked = true;
		surface->popup_acked_x = matched->popup_x;
		surface->popup_acked_y = matched->popup_y;
		surface->popup_acked_width = matched->popup_width;
		surface->popup_acked_height = matched->popup_height;
	}
	/* ack_configure consumes this serial and every older configure. Multiple
	 * acks before a commit are legal; the last one is the state that commit
	 * answers. */
	struct np_xdg_configure *tmp;
	wl_list_for_each_safe(configure, tmp, &surface->xdg_configures, link) {
		bool done = configure == matched;
		wl_list_remove(&configure->link);
		free(configure);
		if (done) break;
	}
	surface->host_configure_acked_serial = serial;
	surface->host_configure_acked = true;
	if (surface->xdg_configure_phase == NP_XDG_AWAITING_INITIAL_ACK)
		surface->xdg_configure_phase = NP_XDG_CONFIGURED;
	if (np_trace_enabled())
		fprintf(stderr,
		        "[configure] ack surface=%u serial=%u current=%u pending=%d\n",
		        surface->id, serial, surface->latest_configure_serial,
		        surface->host_configure_pending);
}

static void xdg_surface_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (surface && (surface->toplevel || surface->popup)) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
		                       "destroy the xdg role object first");
		return;
	}
	wl_resource_destroy(resource);
}

static void xdg_surface_resource_destroy(struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (surface && surface->xdg_surface == resource) {
		np_xdg_clear_configures(surface);
		surface->xdg_surface = NULL;
		surface->xdg_wm_base = NULL;
	}
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
	/* Destroying an xdg_popup/xdg_toplevel object does not clear the permanent
	 * wl_surface role, but clients may construct the same role again on that
	 * wl_surface.  GTK does exactly that when reopening a popover.  Reject only
	 * roles from another protocol here; get_popup/get_toplevel below still
	 * prevents changing between the two xdg roles. */
	bool has_non_xdg_role = surface &&
		surface->role != NP_SURFACE_ROLE_NONE &&
		surface->role != NP_SURFACE_ROLE_XDG_TOPLEVEL &&
		surface->role != NP_SURFACE_ROLE_XDG_POPUP;
	if (!surface || has_non_xdg_role || surface->xdg_surface) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
		                       "wl_surface already has a role");
		return;
	}
	if ((surface->pending_buffer_set && surface->pending_buffer) ||
	    surface->committed_buffer_attached) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
		                       "wl_surface already has attached buffer content");
		return;
	}
	surface->xdg_surface = wl_resource_create(
		client, &xdg_surface_interface, wl_resource_get_version(resource), id);
	if (!surface->xdg_surface) {
		wl_client_post_no_memory(client);
		return;
	}
	surface->xdg_wm_base = resource;
	wl_resource_set_implementation(surface->xdg_surface, &xdg_surface_implementation,
	                               surface, xdg_surface_resource_destroy);
}

static void wm_base_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	struct np_server *server = wl_resource_get_user_data(resource);
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->xdg_wm_base == resource) {
			wl_resource_post_error(resource,
			                       XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,
			                       "destroy xdg surfaces before xdg_wm_base");
			return;
		}
	}
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

void np_xdg_shell_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &xdg_wm_base_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &wm_base_implementation, data, NULL);
}
