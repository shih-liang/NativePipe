// wl_compositor and wl_surface protocol lifecycle.

#define _GNU_SOURCE

#include "compositor_internal.h"
#include "scale.h"
#include "scene.h"
#include "syncobj.h"
#include "window_events.h"
#include "xdg_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-protocol.h>

struct np_surface *np_surface_by_window(struct np_server *server, uint32_t window_id) {
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if ((surface->toplevel || surface->popup) && surface->window_id == window_id) {
			return surface;
		}
	}
	return NULL;
}

struct np_surface *np_surface_by_id(struct np_server *server, uint32_t surface_id) {
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->id == surface_id) return surface;
	}
	return NULL;
}

bool np_surface_assign_role(struct np_surface *surface,
                            enum np_surface_role role) {
	if (!surface || role == NP_SURFACE_ROLE_NONE) return false;
	if (surface->role == NP_SURFACE_ROLE_NONE) surface->role = role;
	return surface->role == role;
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
	if (wl_resource_get_version(resource) >= WL_SURFACE_OFFSET_SINCE_VERSION &&
	    (x != 0 || y != 0)) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
		                       "wl_surface.attach offset requires wl_surface.offset");
		return;
	}
	if (wl_resource_get_version(resource) < WL_SURFACE_OFFSET_SINCE_VERSION) {
		surface->pending_offset_changed = true;
		surface->pending_offset_x = x;
		surface->pending_offset_y = y;
	}
	surface->pending_buffer = buffer;
	surface->pending_buffer_set = true;
	if (np_trace_enabled()) {
		fprintf(stderr, "[wayland] attach surface=%u buffer=%s\n",
		        surface->id, buffer ? "set" : "null");
	}
}

/// Surface-local damage remains in surface coordinates until commit. Requests
/// may freely interleave damage, scale, transform and viewport state.
static void surface_damage(struct wl_client *client, struct wl_resource *resource,
                           int32_t x, int32_t y, int32_t width, int32_t height) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	np_box_union(&surface->pending_surface_damage, x, y, width, height);
}


static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *region) {
	(void)client;
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->pending_opaque_region_changed = true;
	surface->pending_opaque_region_set = region != NULL;
	np_region_fini(&surface->pending_opaque_region);
	if (region && !np_region_copy_resource(region, &surface->pending_opaque_region)) {
		surface->pending_opaque_region_set = false;
		wl_client_post_no_memory(client);
	}
}
static void surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
                                     struct wl_resource *region) {
	(void)client;
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->pending_input_region_changed = true;
	surface->pending_input_region_set = region != NULL;
	np_region_fini(&surface->pending_input_region);
	if (region && !np_region_copy_resource(region, &surface->pending_input_region)) {
		surface->pending_input_region_set = false;
		wl_client_post_no_memory(client);
	}
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
	if (scale <= 0) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE,
		                       "buffer scale must be positive");
		return;
	}
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->pending_scale = scale;
	if (np_trace_enabled())
		fprintf(stderr, "[configure] surface=%u pending buffer scale=%d\n",
		        surface->id, surface->pending_scale);
}

/// Damage already in buffer pixels. This bounds the one copy: a client that
/// repaints a caret should not cost a full-surface memcpy.
static void surface_damage_buffer(struct wl_client *client, struct wl_resource *resource,
                                  int32_t x, int32_t y, int32_t width, int32_t height) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	np_box_union(&surface->pending_buffer_damage, x, y, width, height);
}

static void surface_offset(struct wl_client *client, struct wl_resource *resource,
                           int32_t x, int32_t y) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	surface->pending_offset_changed = true;
	surface->pending_offset_x = x;
	surface->pending_offset_y = y;
}

static const struct wl_surface_interface surface_implementation = {
	.destroy = surface_destroy_handler,
	.attach = surface_attach,
	.damage = surface_damage,
	.frame = np_surface_frame,
	.set_opaque_region = surface_set_opaque_region,
	.set_input_region = surface_set_input_region,
	.commit = np_surface_commit,
	.set_buffer_transform = surface_set_buffer_transform,
	.set_buffer_scale = surface_set_buffer_scale,
	.damage_buffer = surface_damage_buffer,
	.offset = surface_offset,
};

// ---------------------------------------------------------------------------
// wp_fifo_manager_v1
// ---------------------------------------------------------------------------


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
		uint32_t fields[] = {0};
		np_window_event_send(surface->server, NP_GUEST_DRAG_ICON_CHANGED,
		                     fields, 1);
		surface->server->drag_icon = NULL;
	}
	if (surface->server->cursor_surface == resource) {
		uint32_t fields[] = {0, 0, 0};
		np_window_event_send(surface->server, NP_GUEST_CURSOR_CHANGED, fields, 3);
		surface->server->cursor_surface = NULL;
	}
	if (surface->server->pointer_surface == surface->id) {
		surface->server->pointer_surface = 0;
		surface->server->pointer_window = 0;
	}
	if (surface->server->drag_focus_surface == surface->id)
		surface->server->drag_focus_surface = 0;

	// Children may outlive their parent's resource. Remove both active stacking
	// links and unapplied restack operations before any pointer can go stale.
	np_subsurface_detach_tree(surface);
	if (surface->toplevel) {
		uint32_t fields[] = {surface->window_id};
		np_window_event_send(surface->server, NP_GUEST_TOPLEVEL_DESTROYED,
		                     fields, 1);
	}
	if (surface->popup) {
		uint32_t fields[] = {surface->window_id};
		np_window_event_send(surface->server, NP_GUEST_POPUP_DESTROYED,
		                     fields, 1);
	}
	uint32_t destroyed_fields[] = {surface->id};
	np_window_event_send(surface->server, NP_GUEST_SURFACE_DESTROYED,
	                     destroyed_fields, 1);
	if (surface->pending_frame) {
		cJSON_Delete(surface->pending_frame);
		surface->pending_frame = NULL;
	}
	if (surface->host_configure_idle) {
		wl_event_source_remove(surface->host_configure_idle);
		surface->host_configure_idle = NULL;
	}
	np_presentation_clear_scene_wait(surface);
	np_xdg_clear_configures(surface);
	struct np_frame_callback *callback, *callback_tmp;
	wl_list_for_each_safe(callback, callback_tmp, &surface->pending_frame_callbacks, link) {
		wl_resource_destroy(callback->resource);
	}
	struct np_surface_update *update, *update_tmp;
	wl_list_for_each_safe(update, update_tmp, &surface->blocked_updates, link) {
		np_surface_update_destroy(update, true);
	}
	wl_list_for_each_safe(update, update_tmp, &surface->synchronized_updates, link) {
		np_surface_update_destroy(update, true);
	}
	np_presentation_set_current_buffer(surface, NULL, NULL, NULL);
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
	np_region_fini(&surface->pending_input_region);
	np_region_fini(&surface->input_region);
	np_region_fini(&surface->pending_opaque_region);
	np_region_fini(&surface->opaque_region);
#ifndef NP_REMOTE
	np_scene_destroy(surface);
#endif
	wl_list_remove(&surface->link);
	free(surface);
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
	surface->preferred_scale = server->output_scale;
	surface->transform = WL_OUTPUT_TRANSFORM_NORMAL;
	surface->pending_transform = WL_OUTPUT_TRANSFORM_NORMAL;
	surface->scene_wait_fd = -1;
	wl_list_init(&surface->children);
	wl_list_init(&surface->sibling_link);
	wl_list_init(&surface->pending_stack_ops);
	wl_list_init(&surface->pending_frame_callbacks);
	wl_list_init(&surface->blocked_updates);
	wl_list_init(&surface->scene_presentations);
	wl_list_init(&surface->synchronized_updates);
	wl_list_init(&surface->xdg_configures);
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
	uint32_t fields[] = {surface->id};
	np_window_event_send(server, NP_GUEST_SURFACE_CREATED, fields, 1);
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

void np_compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wl_compositor_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &compositor_implementation, data, NULL);
}
