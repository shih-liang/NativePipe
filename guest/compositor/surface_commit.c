// Atomic wl_surface.commit snapshots, synchronization and FIFO ordering.

#define _GNU_SOURCE

#include "compositor_internal.h"
#include "dmabuf.h"
#include "scale.h"
#include "scene.h"
#include "shm_texture.h"
#include "syncobj.h"
#include "window_events.h"
#include "xdg_shell.h"
#include "viewporter-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

static void apply_surface_update_now(struct np_surface_update *update);
static bool parent_has_pending_subsurface_state(struct np_surface *parent);
static bool capture_subsurface_state(struct np_surface_update *update);

static void clear_update_wait(struct np_surface_update *update)
{
	if (!update) return;
	if (update->wait_source) wl_event_source_remove(update->wait_source);
	if (update->wait_fd >= 0) close(update->wait_fd);
	update->wait_source = NULL;
	update->wait_fd = -1;
}


static int retry_dirty_scenes(void *data)
{
	struct np_server *server = data;
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link)
		np_surface_apply_unblocked(surface);
	np_presentation_flush(server);
	wl_display_flush_clients(server->display);
	return 0;
}

void np_surface_schedule_retry(struct np_server *server)
{
	if (!server->scene_retry_timer) {
		struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
		server->scene_retry_timer = wl_event_loop_add_timer(
			loop, retry_dirty_scenes, server);
	}
	if (server->scene_retry_timer)
		wl_event_source_timer_update(server->scene_retry_timer, 8);
}

static int wait_fd_ready(int fd, uint32_t mask, void *data)
{
	(void)fd;
	(void)mask;
	struct np_server *server = data;
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->scene_wait_fd == fd) np_presentation_clear_scene_wait(surface);
		struct np_surface_update *update;
		wl_list_for_each(update, &surface->blocked_updates, link) {
			if (update->wait_fd == fd) clear_update_wait(update);
		}
		wl_list_for_each(update, &surface->synchronized_updates, link) {
			if (update->wait_fd == fd) clear_update_wait(update);
		}
	}
	wl_list_for_each(surface, &server->surfaces, link)
		np_surface_apply_unblocked(surface);
	np_presentation_flush(server);
	np_host_session_sync(server);
	wl_display_flush_clients(server->display);
	return 0;
}

bool np_surface_watch_wait_fd(
	struct np_server *server, int fd,
	struct wl_event_source **source, int *stored_fd)
{
	if (fd < 0 || *source) {
		if (fd >= 0) close(fd);
		return *source != NULL;
	}
	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	*source = wl_event_loop_add_fd(
		loop, fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
		wait_fd_ready, server);
	if (!*source) {
		close(fd);
		return false;
	}
	*stored_fd = fd;
	return true;
}


static bool surface_update_can_apply(
	struct np_surface_update *update, struct np_surface_update *wait_owner) {
	struct np_surface *surface = update->surface;
	if (update->wait_fifo_barrier && surface->fifo_barrier_active)
		return false;
	if (update->acquire_point && !np_sync_point_ready(update->acquire_point)) {
		if (!wait_owner->wait_source) {
			int fd = np_sync_point_wait_fd(update->acquire_point);
			if (!np_surface_watch_wait_fd(surface->server, fd, &wait_owner->wait_source,
			                   &wait_owner->wait_fd))
				np_surface_schedule_retry(surface->server);
		}
		return false;
	}
	struct np_surface_update *dependency;
	wl_list_for_each(dependency, &update->dependencies, link) {
		if (!surface_update_can_apply(dependency, wait_owner)) return false;
	}
	if (update->buffer_commit == NP_BUFFER_UNCHANGED) {
		return true;
	}
	if (update->buffer_commit != NP_BUFFER_ATTACH) return true;
	if (update->gpu_buffer) {
		if (np_gpu_buffer_is_busy(update->gpu_buffer)) return false;
		int fd = -1;
		enum np_gpu_read_result result =
			np_gpu_buffer_render_status(update->gpu_buffer, &fd);
		if (result == NP_GPU_READ_READY) return true;
		if (result == NP_GPU_READ_WAIT) {
			if (!wait_owner->wait_source) {
				if (!np_surface_watch_wait_fd(surface->server, fd, &wait_owner->wait_source,
				                   &wait_owner->wait_fd))
					np_surface_schedule_retry(surface->server);
			} else if (fd >= 0) {
				close(fd);
			}
			return false;
		}
		if (fd >= 0) close(fd);
		wl_client_post_implementation_error(
			wl_resource_get_client(surface->resource),
			"could not wait for the committed linux-dmabuf producer");
		return false;
	}
	if (!update->buffer) return true;
	return true;
}


static void update_buffer_destroyed(struct wl_listener *listener, void *data) {
	(void)data;
	struct np_surface_update *update =
		wl_container_of(listener, update, buffer_destroy);
	update->buffer = NULL;
	wl_list_remove(&listener->link);
	wl_list_init(&listener->link);
}


void np_surface_update_destroy(struct np_surface_update *update, bool release_buffer) {
	if (!update) return;
	clear_update_wait(update);
	if (!wl_list_empty(&update->link)) wl_list_remove(&update->link);
	struct np_surface_update *dependency, *dependency_tmp;
	wl_list_for_each_safe(dependency, dependency_tmp, &update->dependencies, link)
		np_surface_update_destroy(dependency, release_buffer);
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
	np_gpu_buffer_drop(update->gpu_buffer);
	np_region_fini(&update->input_region);
	np_region_fini(&update->opaque_region);
	free(update);
}

static void surface_update_drop_references(struct np_surface_update *update,
	                                        struct np_surface *surface)
{
	struct np_surface_update *dependency, *dependency_tmp;
	wl_list_for_each_safe(dependency, dependency_tmp, &update->dependencies, link) {
		if (dependency->surface == surface) {
			np_surface_update_destroy(dependency, true);
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
void np_surface_drop_queued_references(struct np_server *server,
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
bool np_surface_is_synchronized(struct np_surface *surface) {
	for (struct np_surface *current = surface;
	     current && current->subsurface; current = current->parent) {
		if (current->sync) return true;
	}
	return false;
}

static bool pending_buffer_dimensions(
	struct np_surface *surface, uint32_t *width, uint32_t *height)
{
	if (surface->pending_buffer_set) {
		if (!surface->pending_buffer) return false;
		struct wl_shm_buffer *shm = wl_shm_buffer_get(surface->pending_buffer);
		struct np_gpu_buffer *gpu = np_gpu_buffer_get(surface->pending_buffer);
		if (shm) {
			int32_t w = wl_shm_buffer_get_width(shm);
			int32_t h = wl_shm_buffer_get_height(shm);
			if (w <= 0 || h <= 0) return false;
			*width = (uint32_t)w;
			*height = (uint32_t)h;
			return true;
		}
		if (gpu && gpu->width > 0 && gpu->height > 0) {
			*width = (uint32_t)gpu->width;
			*height = (uint32_t)gpu->height;
			return true;
		}
		return false;
	}
	if (!surface->has_published || surface->last_width <= 0 || surface->last_height <= 0)
		return false;
	*width = (uint32_t)surface->last_width;
	*height = (uint32_t)surface->last_height;
	return true;
}

static bool validate_surface_commit(struct np_surface *surface)
{
	const struct np_viewport_state *viewport = &surface->pending_viewport;
	if (viewport->source_set && !viewport->destination_set &&
	    ((viewport->source_width & 0xff) != 0 ||
	     (viewport->source_height & 0xff) != 0)) {
		if (surface->viewport)
			wl_resource_post_error(surface->viewport, WP_VIEWPORT_ERROR_BAD_SIZE,
			                       "viewport source size must be integral without a destination");
		return false;
	}

	uint32_t width, height;
	if (!pending_buffer_dimensions(surface, &width, &height)) return true;
	struct np_surface_mapping mapping;
	switch (np_scale_resolve_state(
		width, height, surface->pending_scale, surface->pending_transform,
		viewport, &mapping)) {
	case NP_SCALE_OK:
		return true;
	case NP_SCALE_VIEWPORT_BAD_SIZE:
		if (surface->viewport)
			wl_resource_post_error(surface->viewport, WP_VIEWPORT_ERROR_BAD_SIZE,
			                       "invalid viewport source size");
		return false;
	case NP_SCALE_VIEWPORT_OUT_OF_BUFFER:
		if (surface->viewport)
			wl_resource_post_error(surface->viewport, WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
			                       "viewport source extends outside the buffer");
		return false;
	case NP_SCALE_INVALID_SIZE:
		wl_resource_post_error(surface->resource, WL_SURFACE_ERROR_INVALID_SIZE,
		                       "buffer dimensions are not divisible by buffer scale");
		return false;
	}
	return false;
}

static struct np_surface_update *snapshot_surface_update(struct np_surface *surface) {
	bool callbacks = np_presentation_has_unbound_callbacks(surface);
	bool scale_changed = surface->pending_scale != surface->scale;
	bool damaged = surface->pending_surface_damage.width > 0 ||
	               surface->pending_buffer_damage.width > 0;
	bool child_position_changed = parent_has_pending_subsurface_state(surface);
	bool synchronized_children = false;
	struct np_surface *child;
	wl_list_for_each(child, &surface->children, sibling_link) {
		if (!wl_list_empty(&child->synchronized_updates)) {
			synchronized_children = true;
			break;
		}
	}
	bool needs_refresh = surface->pending_buffer_set || damaged || callbacks ||
	                     surface->pending_fifo_set_barrier ||
	                     surface->pending_fifo_wait_barrier ||
	                     surface->pending_viewport_changed ||
	                     surface->pending_transform_changed ||
	                     surface->pending_offset_changed ||
	                     surface->pending_input_region_changed ||
	                     surface->pending_opaque_region_changed ||
	                     np_syncobj_has_pending(surface) ||
	                     surface->pending_geometry_set || scale_changed ||
	                     surface->popup_geometry_acked ||
	                     child_position_changed || synchronized_children;
	if (!needs_refresh && !surface->pending_geometry_set &&
	    !scale_changed && !child_position_changed &&
	    !surface->pending_size_constraints_changed &&
	    !surface->host_configure_acked)
		return NULL;

	struct np_surface_update *update = calloc(1, sizeof(*update));
	if (!update) return NULL;
	update->wait_fd = -1;
	wl_list_init(&update->link);
	wl_list_init(&update->dependencies);
	wl_list_init(&update->subsurface_positions);
	wl_list_init(&update->stack_ops);
	wl_list_init(&update->buffer_destroy.link);
	update->surface = surface;
	update->host_configure_serial = surface->host_configure_acked
		? surface->host_configure_acked_host_serial : 0;
	update->buffer = surface->pending_buffer;
	update->buffer_commit = !surface->pending_buffer_set ? NP_BUFFER_UNCHANGED
		: surface->pending_buffer ? NP_BUFFER_ATTACH : NP_BUFFER_DETACH;
	update->gpu_buffer = np_gpu_buffer_get(surface->pending_buffer);
	np_gpu_buffer_retain(update->gpu_buffer);
	update->scale = surface->pending_scale;
	update->geometry_set = surface->pending_geometry_set;
	update->geometry_x = surface->pending_geometry_x;
	update->geometry_y = surface->pending_geometry_y;
	update->geometry_width = surface->pending_geometry_width;
	update->geometry_height = surface->pending_geometry_height;
	update->popup_geometry_changed = surface->popup_geometry_acked;
	update->popup_x = surface->popup_acked_x;
	update->popup_y = surface->popup_acked_y;
	update->popup_width = surface->popup_acked_width;
	update->popup_height = surface->popup_acked_height;
	update->viewport_changed = surface->pending_viewport_changed;
	update->viewport = surface->pending_viewport;
	update->transform_changed = surface->pending_transform_changed;
	update->transform = surface->pending_transform;
	update->offset_changed = surface->pending_offset_changed;
	update->offset_x = surface->pending_offset_x;
	update->offset_y = surface->pending_offset_y;
	update->input_region_changed = surface->pending_input_region_changed;
	update->input_region_set = surface->pending_input_region_set;
	update->opaque_region_changed = surface->pending_opaque_region_changed;
	update->opaque_region_set = surface->pending_opaque_region_set;
	update->size_constraints_changed = surface->pending_size_constraints_changed;
	update->minimum_width = surface->pending_minimum_width;
	update->minimum_height = surface->pending_minimum_height;
	update->maximum_width = surface->pending_maximum_width;
	update->maximum_height = surface->pending_maximum_height;
	update->damage = surface->pending_buffer_damage;
	uint32_t buffer_width, buffer_height;
	struct np_box converted_damage;
	np_box_clear(&converted_damage);
	if (pending_buffer_dimensions(surface, &buffer_width, &buffer_height) &&
	    np_scale_damage_to_buffer(
		    buffer_width, buffer_height, surface->pending_scale,
		    surface->pending_transform, &surface->pending_viewport,
		    &surface->pending_surface_damage, &converted_damage))
		np_box_union(&update->damage, converted_damage.x, converted_damage.y,
		          converted_damage.width, converted_damage.height);
	if (np_trace_enabled() && damaged)
		fprintf(stderr,
		        "[damage] surface=%u upload=%lld,%lld %lldx%lld "
		        "surface=%lld,%lld %lldx%lld buffer=%lld,%lld %lldx%lld\n",
		        surface->id,
		        (long long)update->damage.x, (long long)update->damage.y,
		        (long long)update->damage.width, (long long)update->damage.height,
		        (long long)surface->pending_surface_damage.x,
		        (long long)surface->pending_surface_damage.y,
		        (long long)surface->pending_surface_damage.width,
		        (long long)surface->pending_surface_damage.height,
		        (long long)surface->pending_buffer_damage.x,
		        (long long)surface->pending_buffer_damage.y,
		        (long long)surface->pending_buffer_damage.width,
		        (long long)surface->pending_buffer_damage.height);
	update->set_fifo_barrier = surface->pending_fifo_set_barrier;
	update->wait_fifo_barrier = surface->pending_fifo_wait_barrier;
	if (!np_syncobj_take_commit(
			surface, update->buffer_commit != NP_BUFFER_UNCHANGED, update->buffer,
			&update->acquire_point, &update->release_point)) {
		np_surface_update_destroy(update, false);
		return NULL;
	}
	if (!capture_subsurface_state(update)) {
		np_surface_update_destroy(update, false);
		wl_client_post_no_memory(wl_resource_get_client(surface->resource));
		return NULL;
	}
	if (update->buffer) {
		update->buffer_destroy.notify = update_buffer_destroyed;
		wl_resource_add_destroy_listener(update->buffer, &update->buffer_destroy);
	}
	if (update->input_region_changed)
		np_region_move(&update->input_region, &surface->pending_input_region);
	if (update->opaque_region_changed)
		np_region_move(&update->opaque_region, &surface->pending_opaque_region);

	surface->pending_buffer = NULL;
	surface->pending_buffer_set = false;
	surface->pending_geometry_set = false;
	surface->popup_geometry_acked = false;
	surface->pending_viewport_changed = false;
	surface->pending_transform_changed = false;
	surface->pending_offset_changed = false;
	surface->pending_input_region_changed = false;
	surface->pending_opaque_region_changed = false;
	surface->pending_size_constraints_changed = false;
	np_box_clear(&surface->pending_surface_damage);
	np_box_clear(&surface->pending_buffer_damage);

	surface->pending_fifo_set_barrier = false;
	surface->pending_fifo_wait_barrier = false;
	surface->host_configure_acked = false;

	if (needs_refresh) {
		update->presentation_id = np_presentation_next_id(surface->server);
		np_presentation_bind_callbacks(surface, update->presentation_id);
	}

	return update;
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
	clear_update_wait(update);
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
	if (update->host_configure_serial)
		surface->committed_host_configure_serial = update->host_configure_serial;
	bool scale_changed = surface->scale != update->scale;
	surface->scale = update->scale;
	if (update->geometry_set) {
		surface->geometry_set = true;
		surface->geometry_x = update->geometry_x;
		surface->geometry_y = update->geometry_y;
		surface->geometry_width = update->geometry_width;
		surface->geometry_height = update->geometry_height;
	}
	if (update->popup_geometry_changed)
		np_xdg_apply_popup_geometry(surface, update->popup_x, update->popup_y,
		                            update->popup_width, update->popup_height);
	if (update->viewport_changed)
		surface->viewport_state = update->viewport;
	if (update->transform_changed)
		surface->transform = update->transform;
	if (update->offset_changed) {
		surface->buffer_offset_x += update->offset_x;
		surface->buffer_offset_y += update->offset_y;
		if (surface->role == NP_SURFACE_ROLE_CURSOR &&
		    surface->server->cursor_surface == surface->resource) {
			surface->server->cursor_hotspot_x -= update->offset_x;
			surface->server->cursor_hotspot_y -= update->offset_y;
			uint32_t fields[] = {
				surface->id,
				(uint32_t)surface->server->cursor_hotspot_x,
				(uint32_t)surface->server->cursor_hotspot_y,
			};
			np_window_event_send(surface->server, NP_GUEST_CURSOR_CHANGED,
			                     fields, 3);
		}
	}
	if (update->input_region_changed) {
		surface->input_region_set = update->input_region_set;
		np_region_move(&surface->input_region, &update->input_region);
	}
	if (update->opaque_region_changed) {
		surface->opaque_region_set = update->opaque_region_set;
		np_region_move(&surface->opaque_region, &update->opaque_region);
	}
	if (update->size_constraints_changed) {
		np_xdg_apply_size_constraints(
			surface, update->minimum_width, update->minimum_height,
			update->maximum_width, update->maximum_height);
	}
	uint32_t content_width = surface->last_width > 0
		? (uint32_t)surface->last_width : 0;
	uint32_t content_height = surface->last_height > 0
		? (uint32_t)surface->last_height : 0;
	if (update->buffer_commit == NP_BUFFER_DETACH) {
		content_width = content_height = 0;
	} else if (update->buffer_commit == NP_BUFFER_ATTACH) {
		struct wl_shm_buffer *shm = update->buffer
			? wl_shm_buffer_get(update->buffer) : NULL;
		if (shm) {
			int32_t width = wl_shm_buffer_get_width(shm);
			int32_t height = wl_shm_buffer_get_height(shm);
			content_width = width > 0 ? (uint32_t)width : 0;
			content_height = height > 0 ? (uint32_t)height : 0;
		} else if (update->gpu_buffer) {
			content_width = update->gpu_buffer->width > 0
				? (uint32_t)update->gpu_buffer->width : 0;
			content_height = update->gpu_buffer->height > 0
				? (uint32_t)update->gpu_buffer->height : 0;
		} else {
			content_width = content_height = 0;
		}
	}
	np_shm_texture_prepare_commit(
		surface, content_width, content_height,
		update->viewport_changed || update->transform_changed || scale_changed);
	bool scene_structure_changed =
		update->buffer_commit == NP_BUFFER_DETACH || update->geometry_set ||
		update->popup_geometry_changed || update->viewport_changed ||
		update->transform_changed || update->offset_changed || scale_changed ||
		update->subsurface_state_changed ||
		!wl_list_empty(&update->subsurface_positions) ||
		!wl_list_empty(&update->stack_ops);
	np_scene_note_damage(surface, &update->damage, scene_structure_changed);
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
	if (np_trace_enabled()) {
		fprintf(stderr, "[wayland] apply commit surface=%u buffer=%s present=%u%s\n",
		        surface->id,
		        update->buffer_commit == NP_BUFFER_ATTACH ? "set" :
		        update->buffer_commit == NP_BUFFER_DETACH ? "gone" : "unchanged",
		        update->presentation_id, update->set_fifo_barrier ? " fifo" : "");
	}

	if (update->buffer_commit != NP_BUFFER_UNCHANGED) {
		if (np_scene_root(surface)) {
			np_presentation_publish_buffer(surface, update->buffer, update->gpu_buffer,
			                       update->buffer_commit, update->presentation_id,
			                       update->release_point, &update->damage);
			update->release_point = NULL;
		} else if (update->buffer || update->buffer_commit == NP_BUFFER_DETACH) {
			np_presentation_publish_buffer(surface, update->buffer, update->gpu_buffer,
			                       update->buffer_commit, update->presentation_id,
			                       update->release_point, &update->damage);
			update->release_point = NULL;
		} else if (update->buffer_commit == NP_BUFFER_ATTACH && update->gpu_buffer) {
			np_presentation_publish_buffer(surface, NULL, update->gpu_buffer,
			                       NP_BUFFER_ATTACH, update->presentation_id,
			                       update->release_point, &update->damage);
			update->release_point = NULL;
		} else if (update->presentation_id) {
			np_presentation_request_refresh(surface, update->presentation_id);
		}
	} else if (update->damage.width > 0 && surface->current_buffer &&
	           wl_shm_buffer_get(surface->current_buffer)) {
		(void)np_presentation_refresh_current_shm(
			surface, update->presentation_id, &update->damage);
	} else if (update->presentation_id) {
		if (!((update->viewport_changed || update->geometry_set || scale_changed ||
		       update->offset_changed ||
		       update->subsurface_state_changed) &&
		      np_presentation_queue_last(surface, update->presentation_id)))
			np_presentation_request_refresh(surface, update->presentation_id);
	}
	np_shm_texture_note_damage(surface, &update->damage);

	if (update->set_fifo_barrier) {
		surface->fifo_barrier_active = true;
		struct np_surface *root = np_scene_root(surface);
		surface->fifo_barrier_presentation_id =
			root && root->scene_dirty
				? root->scene_presentation_id : update->presentation_id;
	}
	np_surface_update_destroy(update, false);
}

void np_surface_apply_update(struct np_surface_update *update) {
	if (!update) return;
	if (np_surface_is_synchronized(update->surface)) {
		wl_list_insert(update->surface->synchronized_updates.prev, &update->link);
		return;
	}
	if (!wl_list_empty(&update->surface->blocked_updates) ||
	    !surface_update_can_apply(update, update)) {
		if (np_trace_enabled())
			fprintf(stderr,
			        "[wayland] queue blocked surface=%u present=%u fifo=%d gpu_busy=%d acquire=%d\n",
			        update->surface->id, update->presentation_id,
			        update->wait_fifo_barrier &&
			        update->surface->fifo_barrier_active,
			        update->gpu_buffer && np_gpu_buffer_is_busy(update->gpu_buffer),
			        update->acquire_point &&
			        !np_sync_point_ready(update->acquire_point));
		wl_list_insert(update->surface->blocked_updates.prev, &update->link);
		return;
	}
	apply_surface_update_now(update);
}

void np_surface_apply_unblocked(struct np_surface *surface) {
	while (!wl_list_empty(&surface->blocked_updates)) {
		struct np_surface_update *update =
			wl_container_of(surface->blocked_updates.next, update, link);
		if (!surface_update_can_apply(update, update)) break;
		wl_list_remove(&update->link);
		wl_list_init(&update->link);
		apply_surface_update_now(update);
	}
}

void np_surface_commit(struct wl_client *client, struct wl_resource *resource) {
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!validate_surface_commit(surface)) return;
	bool xdg_role = surface->toplevel || surface->popup;
	bool attaching_buffer = surface->pending_buffer_set && surface->pending_buffer;

	/* xdg-shell's initial commit is a distinct protocol phase even though it
	 * arrives through the same wl_surface.commit request. It commits surface
	 * state, sends the role's first configure, and never falls through to the
	 * mapped-buffer path. */
	if (xdg_role &&
	    surface->xdg_configure_phase == NP_XDG_AWAITING_INITIAL_COMMIT) {
		if (attaching_buffer) {
			wl_resource_post_error(surface->xdg_surface,
			                       XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
			                       "buffer committed before initial configure");
			return;
		}
		if (surface->pending_buffer_set)
			surface->committed_buffer_attached = false;
		struct np_surface_update *initial = snapshot_surface_update(surface);
		if (initial) {
			/* attach(NULL) is also a valid initial bufferless commit, but it is
			 * not an unmap operation: no mapped content exists in this phase. */
			initial->buffer_commit = NP_BUFFER_UNCHANGED;
			np_surface_apply_update(initial);
		}
		surface->xdg_configure_phase = NP_XDG_AWAITING_INITIAL_ACK;
		np_xdg_send_initial_role_configure(surface);
		return;
	}

	if (xdg_role && attaching_buffer &&
	    surface->xdg_configure_phase != NP_XDG_CONFIGURED) {
		wl_resource_post_error(surface->xdg_surface,
		                       XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
		                       "buffer committed before initial configure was acknowledged");
		return;
	}

	bool unmapping = xdg_role && surface->committed_buffer_attached &&
	                 surface->pending_buffer_set && !surface->pending_buffer;
	/* Capture this before snapshot_surface_update() consumes the pending ack.
	 * The xdg configure state becomes current at this commit boundary. Output
	 * presentation is independent and must not throttle later resize configures. */
	uint32_t configure_serial = surface->host_configure_acked
		? surface->host_configure_acked_serial : 0;
	if (surface->pending_buffer_set)
		surface->committed_buffer_attached = surface->pending_buffer != NULL;
	struct np_surface_update *update = snapshot_surface_update(surface);
	if (configure_serial)
		np_xdg_finish_toplevel_configure(surface, configure_serial);
	if (update) np_surface_apply_update(update);
	if (unmapping) {
		surface->xdg_configure_phase = NP_XDG_AWAITING_INITIAL_COMMIT;
		np_xdg_clear_configures(surface);
	}
}
