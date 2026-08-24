// wl_subcompositor role, parenting, position and stacking requests.

#include "compositor_internal.h"
#include "scene.h"
#include "window_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <wayland-server-protocol.h>

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

void np_subsurface_detach(struct np_surface *surface)
{
	if (!surface) return;
	np_surface_drop_queued_references(surface->server, surface);
	drop_stack_ops_referencing(surface->server, surface);
	if (surface->parent) {
		wl_list_remove(&surface->sibling_link);
		wl_list_init(&surface->sibling_link);
		surface->parent = NULL;
	}
}

void np_subsurface_detach_tree(struct np_surface *surface)
{
	if (!surface) return;
	np_subsurface_detach(surface);
	struct np_surface *child, *tmp;
	wl_list_for_each_safe(child, tmp, &surface->children, sibling_link) {
		wl_list_remove(&child->sibling_link);
		wl_list_init(&child->sibling_link);
		child->parent = NULL;
	}
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
	if (np_trace_enabled())
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
	if (np_trace_enabled())
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
	if (!np_surface_is_synchronized(surface)) {
		while (!wl_list_empty(&surface->synchronized_updates)) {
			struct np_surface_update *update = wl_container_of(
				surface->synchronized_updates.next, update, link);
			wl_list_remove(&update->link);
			wl_list_init(&update->link);
			np_surface_apply_update(update);
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
	struct np_surface *root = np_scene_root(surface);
	surface->subsurface = NULL;
	np_subsurface_detach(surface);
	/* Destroying wl_subsurface unmaps it immediately. Re-publish the former
	 * root without waiting for an unrelated parent commit. The permanent role
	 * remains SUBSURFACE, so the client may create a new role object later. */
	if (root && root != surface)
		np_presentation_queue_scene(root, np_presentation_next_id(surface->server));
	uint32_t fields[] = {surface->id};
	np_window_event_send(surface->server, NP_GUEST_SUBSURFACE_DESTROYED, fields, 1);
}

static void subcompositor_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void subcompositor_get_subsurface(struct wl_client *client, struct wl_resource *resource,
                                         uint32_t id, struct wl_resource *surface_resource,
                                         struct wl_resource *parent_resource) {
	struct np_surface *surface = wl_resource_get_user_data(surface_resource);
	struct np_surface *parent = wl_resource_get_user_data(parent_resource);
	if (!surface || surface->subsurface) {
		wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
		                       "wl_surface already has another role object");
		return;
	}
	for (struct np_surface *ancestor = parent; ancestor; ancestor = ancestor->parent) {
		if (ancestor != surface) continue;
		wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
		                       "subsurface parent creates a cycle");
		return;
	}
	if (!np_surface_assign_role(surface, NP_SURFACE_ROLE_SUBSURFACE)) {
		wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
		                       "wl_surface already has another role");
		return;
	}

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
	if (np_trace_enabled())
		fprintf(stderr, "[wayland] subsurface created surface=%u parent=%u\n",
		        surface->id, parent->id);
	uint32_t fields[] = {surface->id, parent->id, 0, 0};
	np_window_event_send(surface->server, NP_GUEST_SUBSURFACE_CREATED, fields, 4);
}

static const struct wl_subcompositor_interface subcompositor_implementation = {
	.destroy = subcompositor_destroy_handler,
	.get_subsurface = subcompositor_get_subsurface,
};

void np_subcompositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wl_subcompositor_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &subcompositor_implementation, data, NULL);
}
