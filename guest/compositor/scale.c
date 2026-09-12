#include "scale.h"
#include "compositor_internal.h"
#include "fractional-scale-v1-server-protocol.h"
#include "scene.h"
#include "viewporter-server-protocol.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-protocol.h>

#define NP_SCALE_UNIT 120

bool np_scale_transform_swaps_axes(int32_t transform)
{
	return transform == WL_OUTPUT_TRANSFORM_90 ||
	       transform == WL_OUTPUT_TRANSFORM_270 ||
	       transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
	       transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
}

/* Map a normalized point in the transformed/surface-facing buffer back to
 * the client's original texture. These are the same eight mappings consumed
 * by HostSceneRenderer, making crop, damage and sampling one coordinate model. */
static void transformed_to_source(int32_t transform, double u, double v,
	                              double *source_u, double *source_v)
{
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_90: *source_u = v; *source_v = 1.0 - u; break;
	case WL_OUTPUT_TRANSFORM_180: *source_u = 1.0 - u; *source_v = 1.0 - v; break;
	case WL_OUTPUT_TRANSFORM_270: *source_u = 1.0 - v; *source_v = u; break;
	case WL_OUTPUT_TRANSFORM_FLIPPED: *source_u = 1.0 - u; *source_v = v; break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90: *source_u = v; *source_v = u; break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180: *source_u = u; *source_v = 1.0 - v; break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270: *source_u = 1.0 - v; *source_v = 1.0 - u; break;
	default: *source_u = u; *source_v = v; break;
	}
}

static void transformed_rect_to_source(
	int32_t transform, double transformed_width, double transformed_height,
	double source_width, double source_height,
	double x, double y, double width, double height,
	double *out_x, double *out_y, double *out_width, double *out_height)
{
	double min_x = source_width, min_y = source_height, max_x = 0, max_y = 0;
	for (int corner = 0; corner < 4; corner++) {
		double tx = x + ((corner & 1) ? width : 0);
		double ty = y + ((corner & 2) ? height : 0);
		double u, v;
		transformed_to_source(transform, tx / transformed_width,
		                      ty / transformed_height, &u, &v);
		double sx = u * source_width, sy = v * source_height;
		if (sx < min_x) min_x = sx;
		if (sy < min_y) min_y = sy;
		if (sx > max_x) max_x = sx;
		if (sy > max_y) max_y = sy;
	}
	*out_x = min_x; *out_y = min_y;
	*out_width = max_x - min_x; *out_height = max_y - min_y;
}

enum np_scale_error np_scale_resolve_state(
	uint32_t buffer_width, uint32_t buffer_height, int32_t scale,
	int32_t transform, const struct np_viewport_state *viewport,
	struct np_surface_mapping *mapping)
{
	if (!mapping || !viewport || !buffer_width || !buffer_height || scale <= 0)
		return NP_SCALE_INVALID_SIZE;

	double transformed_width = np_scale_transform_swaps_axes(transform)
		? buffer_height : buffer_width;
	double transformed_height = np_scale_transform_swaps_axes(transform)
		? buffer_width : buffer_height;
	double logical_source_x = 0;
	double logical_source_y = 0;
	double logical_source_width = transformed_width / scale;
	double logical_source_height = transformed_height / scale;
	if (viewport->source_set) {
		logical_source_x = wl_fixed_to_double(viewport->source_x);
		logical_source_y = wl_fixed_to_double(viewport->source_y);
		logical_source_width = wl_fixed_to_double(viewport->source_width);
		logical_source_height = wl_fixed_to_double(viewport->source_height);
		if (!viewport->destination_set &&
		    ((viewport->source_width & 0xff) != 0 ||
		     (viewport->source_height & 0xff) != 0))
			return NP_SCALE_VIEWPORT_BAD_SIZE;
	}
	if (logical_source_x < 0 || logical_source_y < 0 ||
	    logical_source_width <= 0 || logical_source_height <= 0)
		return NP_SCALE_VIEWPORT_BAD_SIZE;
	if ((logical_source_x + logical_source_width) * scale > transformed_width + 0.001 ||
	    (logical_source_y + logical_source_height) * scale > transformed_height + 0.001)
		return NP_SCALE_VIEWPORT_OUT_OF_BUFFER;
	if (!viewport->destination_set && !viewport->source_set &&
	    ((uint32_t)transformed_width % (uint32_t)scale != 0 ||
	     (uint32_t)transformed_height % (uint32_t)scale != 0))
		return NP_SCALE_INVALID_SIZE;

	transformed_rect_to_source(
		transform, transformed_width, transformed_height,
		buffer_width, buffer_height,
		logical_source_x * scale, logical_source_y * scale,
		logical_source_width * scale, logical_source_height * scale,
		&mapping->source_x_pixels, &mapping->source_y_pixels,
		&mapping->source_width_pixels, &mapping->source_height_pixels);
	if (viewport->destination_set) {
		mapping->logical_width = viewport->destination_width;
		mapping->logical_height = viewport->destination_height;
	} else {
		mapping->logical_width = logical_source_width;
		mapping->logical_height = logical_source_height;
	}
	return mapping->logical_width > 0 && mapping->logical_height > 0
		? NP_SCALE_OK : NP_SCALE_INVALID_SIZE;
}

bool np_scale_resolve(const struct np_surface *surface,
	                  uint32_t buffer_width, uint32_t buffer_height,
	                  struct np_surface_mapping *mapping)
{
	return surface && np_scale_resolve_state(
		buffer_width, buffer_height, surface->scale, surface->transform,
		&surface->viewport_state, mapping) == NP_SCALE_OK;
}

bool np_scale_damage_to_buffer(
	uint32_t buffer_width, uint32_t buffer_height, int32_t scale,
	int32_t transform, const struct np_viewport_state *viewport,
	const struct np_box *surface_damage, struct np_box *buffer_damage)
{
	if (!viewport || !surface_damage || !buffer_damage ||
	    surface_damage->width <= 0 || surface_damage->height <= 0)
		return false;
	struct np_surface_mapping mapping;
	if (np_scale_resolve_state(buffer_width, buffer_height, scale, transform,
	                           viewport, &mapping) != NP_SCALE_OK)
		return false;

	double surface_width = mapping.logical_width;
	double surface_height = mapping.logical_height;
	double x0 = fmax(0.0, (double)surface_damage->x);
	double y0 = fmax(0.0, (double)surface_damage->y);
	double x1 = fmin(surface_width,
	                 (double)surface_damage->x + (double)surface_damage->width);
	double y1 = fmin(surface_height,
	                 (double)surface_damage->y + (double)surface_damage->height);
	if (x1 <= x0 || y1 <= y0) return false;

	double transformed_width = np_scale_transform_swaps_axes(transform)
		? buffer_height : buffer_width;
	double transformed_height = np_scale_transform_swaps_axes(transform)
		? buffer_width : buffer_height;
	double logical_source_x = viewport->source_set
		? wl_fixed_to_double(viewport->source_x) : 0.0;
	double logical_source_y = viewport->source_set
		? wl_fixed_to_double(viewport->source_y) : 0.0;
	double logical_source_width = viewport->source_set
		? wl_fixed_to_double(viewport->source_width) : transformed_width / scale;
	double logical_source_height = viewport->source_set
		? wl_fixed_to_double(viewport->source_height) : transformed_height / scale;
	double tx = (logical_source_x + x0 * logical_source_width / surface_width) * scale;
	double ty = (logical_source_y + y0 * logical_source_height / surface_height) * scale;
	double tw = (x1 - x0) * logical_source_width / surface_width * scale;
	double th = (y1 - y0) * logical_source_height / surface_height * scale;
	double bx, by, bw, bh;
	transformed_rect_to_source(
		transform, transformed_width, transformed_height,
		buffer_width, buffer_height,
		tx, ty, tw, th,
		&bx, &by, &bw, &bh);
	double left = fmax(0.0, floor(bx + 0.0001));
	double top = fmax(0.0, floor(by + 0.0001));
	double right = fmin((double)buffer_width, ceil(bx + bw - 0.0001));
	double bottom = fmin((double)buffer_height, ceil(by + bh - 0.0001));
	if (right <= left || bottom <= top) return false;
	buffer_damage->x = (int64_t)left;
	buffer_damage->y = (int64_t)top;
	buffer_damage->width = (int64_t)(right - left);
	buffer_damage->height = (int64_t)(bottom - top);
	return true;
}

static void viewport_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void viewport_set_source(struct wl_client *client, struct wl_resource *resource,
	                            wl_fixed_t x, wl_fixed_t y,
	                            wl_fixed_t width, wl_fixed_t height)
{
	(void)client;
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
		                       "the wl_surface no longer exists");
		return;
	}
	wl_fixed_t unset = wl_fixed_from_int(-1);
	if (x == unset && y == unset && width == unset && height == unset) {
		surface->pending_viewport.source_set = false;
		surface->pending_viewport_changed = true;
		return;
	}
	if (x < 0 || y < 0 || width <= 0 || height <= 0) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
		                       "viewport source must be positive or entirely unset");
		return;
	}
	surface->pending_viewport.source_set = true;
	surface->pending_viewport.source_x = x;
	surface->pending_viewport.source_y = y;
	surface->pending_viewport.source_width = width;
	surface->pending_viewport.source_height = height;
	surface->pending_viewport_changed = true;
}

static void viewport_set_destination(struct wl_client *client,
	                                 struct wl_resource *resource,
	                                 int32_t width, int32_t height)
{
	(void)client;
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
		                       "the wl_surface no longer exists");
		return;
	}
	if (width == -1 && height == -1) {
		surface->pending_viewport.destination_set = false;
		surface->pending_viewport_changed = true;
		return;
	}
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
		                       "viewport destination must be positive or entirely unset");
		return;
	}
	surface->pending_viewport.destination_set = true;
	surface->pending_viewport.destination_width = width;
	surface->pending_viewport.destination_height = height;
	surface->pending_viewport_changed = true;
}

static const struct wp_viewport_interface viewport_implementation = {
	.destroy = viewport_destroy,
	.set_source = viewport_set_source,
	.set_destination = viewport_set_destination,
};

static void viewport_resource_destroy(struct wl_resource *resource)
{
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface || surface->viewport != resource)
		return;
	surface->viewport = NULL;
	memset(&surface->pending_viewport, 0, sizeof(surface->pending_viewport));
	surface->pending_viewport_changed = true;
}

static void viewporter_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void viewporter_get_viewport(struct wl_client *client,
	                               struct wl_resource *resource, uint32_t id,
	                               struct wl_resource *surface_resource)
{
	struct np_surface *surface = wl_resource_get_user_data(surface_resource);
	if (!surface) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
		                       "the wl_surface no longer exists");
		return;
	}
	if (surface->viewport) {
		wl_resource_post_error(resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
		                       "the wl_surface already has a viewport");
		return;
	}
	struct wl_resource *viewport = wl_resource_create(
		client, &wp_viewport_interface, wl_resource_get_version(resource), id);
	if (!viewport) {
		wl_client_post_no_memory(client);
		return;
	}
	surface->viewport = viewport;
	wl_resource_set_implementation(viewport, &viewport_implementation,
	                               surface, viewport_resource_destroy);
}

static const struct wp_viewporter_interface viewporter_implementation = {
	.destroy = viewporter_destroy,
	.get_viewport = viewporter_get_viewport,
};

static void viewporter_bind(struct wl_client *client, void *data,
	                       uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *resource = wl_resource_create(
		client, &wp_viewporter_interface, version > 1 ? 1 : version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &viewporter_implementation, NULL, NULL);
}

static void send_preferred_scale(struct np_surface *surface, int scale)
{
	if (!surface || !surface->fractional_scale || scale <= 0 ||
	    surface->reported_scale == scale)
		return;
	surface->reported_scale = scale;
	wp_fractional_scale_v1_send_preferred_scale(
		surface->fractional_scale, (uint32_t)(scale * NP_SCALE_UNIT));
}

static void fractional_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wp_fractional_scale_v1_interface fractional_implementation = {
	.destroy = fractional_destroy,
};

static void fractional_resource_destroy(struct wl_resource *resource)
{
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (surface && surface->fractional_scale == resource)
		surface->fractional_scale = NULL;
}

static void fractional_manager_destroy(struct wl_client *client,
	                                   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void fractional_manager_get_scale(struct wl_client *client,
	                                     struct wl_resource *resource,
	                                     uint32_t id,
	                                     struct wl_resource *surface_resource)
{
	struct np_surface *surface = wl_resource_get_user_data(surface_resource);
	if (!surface) return;
	if (surface->fractional_scale) {
		wl_resource_post_error(
			resource,
			WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
			"the wl_surface already has a fractional scale object");
		return;
	}
	struct wl_resource *scale = wl_resource_create(
		client, &wp_fractional_scale_v1_interface,
		wl_resource_get_version(resource), id);
	if (!scale) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(scale, &fractional_implementation,
	                               surface, fractional_resource_destroy);
	surface->fractional_scale = scale;
	surface->reported_scale = 0;
	send_preferred_scale(surface, surface->preferred_scale);
}

static const struct wp_fractional_scale_manager_v1_interface fractional_manager_implementation = {
	.destroy = fractional_manager_destroy,
	.get_fractional_scale = fractional_manager_get_scale,
};

static void fractional_manager_bind(struct wl_client *client, void *data,
	                                uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(
		client, &wp_fractional_scale_manager_v1_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &fractional_manager_implementation,
	                               data, NULL);
}

struct np_output_state {
	struct wl_list link;
	struct np_server *server;
	struct wl_global *global;
	uint32_t id;
	char *name;
	int32_t x, y, width, height;
	int32_t pixel_width, pixel_height;
	int32_t physical_width_mm, physical_height_mm;
	int32_t scale, refresh_millihz;
};

static struct np_output_state *output_state_by_id(
	struct np_server *server, uint32_t id)
{
	struct np_output_state *state;
	wl_list_for_each(state, &server->output_states, link) {
		if (state->global && state->id == id) return state;
	}
	return NULL;
}

static struct np_output_state *first_output_state(struct np_server *server)
{
	struct np_output_state *state;
	wl_list_for_each(state, &server->output_states, link) {
		if (state->global) return state;
	}
	return NULL;
}

static bool output_state_has_resources(const struct np_output_state *state)
{
	struct np_output *output;
	wl_list_for_each(output, &state->server->outputs, link) {
		if (output->state == state) return true;
	}
	return false;
}

int32_t np_scale_surface_refresh_millihz(const struct np_surface *surface)
{
    struct np_output_state *state = output_state_by_id(surface->server, surface->output_id);
    if (!state) state = first_output_state(surface->server);
    return state && state->refresh_millihz > 0 ? state->refresh_millihz : 60000;
}

static void output_state_maybe_destroy(struct np_output_state *state)
{
	if (!state || state->global || output_state_has_resources(state)) return;
	wl_list_remove(&state->link);
	free(state->name);
	free(state);
}

static void output_resource_destroy(struct wl_resource *resource)
{
	struct np_output *output = wl_resource_get_user_data(resource);
	if (!output) return;
	struct np_output_state *state = output->state;
	wl_list_remove(&output->link);
	free(output);
	output_state_maybe_destroy(state);
}

static void output_release(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_output_interface output_implementation = {
	.release = output_release,
};

static void output_send_state(
	const struct np_output_state *state, struct wl_resource *resource)
{
	uint32_t version = (uint32_t)wl_resource_get_version(resource);
	wl_output_send_geometry(
		resource, state->x, state->y,
		state->physical_width_mm, state->physical_height_mm,
		WL_OUTPUT_SUBPIXEL_UNKNOWN, "Apple", state->name,
		WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(
		resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
		state->pixel_width, state->pixel_height, state->refresh_millihz);
	if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
		wl_output_send_scale(resource, state->scale);
	if (version >= WL_OUTPUT_NAME_SINCE_VERSION)
		wl_output_send_name(resource, state->name);
	if (version >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION)
		wl_output_send_description(resource, state->name);
	if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
		wl_output_send_done(resource);
}

static void output_bind(struct wl_client *client, void *data,
	                   uint32_t version, uint32_t id)
{
	struct np_output_state *state = data;
	struct np_server *server = state->server;
	struct wl_resource *resource = wl_resource_create(
		client, &wl_output_interface, (int)(version > 4 ? 4 : version), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_output *output = calloc(1, sizeof(*output));
	if (!output) {
		wl_resource_destroy(resource);
		wl_client_post_no_memory(client);
		return;
	}
	output->resource = resource;
	output->state = state;
	wl_list_insert(&server->outputs, &output->link);
	wl_resource_set_implementation(
		resource, &output_implementation, output, output_resource_destroy);
	output_send_state(state, resource);

	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->output_id == state->id &&
		    wl_resource_get_client(surface->resource) == client)
			wl_surface_send_enter(surface->resource, resource);
	}
}

static struct np_output_state *output_state_create(
	struct np_server *server, const struct np_host_output *value)
{
	struct np_output_state *state = calloc(1, sizeof(*state));
	if (!state) return NULL;
	state->name = strdup(value->name);
	if (!state->name) {
		free(state);
		return NULL;
	}
	state->server = server;
	state->id = value->id;
	state->x = value->x;
	state->y = value->y;
	state->width = value->width;
	state->height = value->height;
	state->pixel_width = value->pixel_width;
	state->pixel_height = value->pixel_height;
	state->physical_width_mm = value->physical_width_mm;
	state->physical_height_mm = value->physical_height_mm;
	state->scale = value->scale;
	state->refresh_millihz = value->refresh_millihz;
	wl_list_insert(server->output_states.prev, &state->link);
	state->global = wl_global_create(
		server->display, &wl_output_interface, 4, state, output_bind);
	if (!state->global) {
		wl_list_remove(&state->link);
		free(state->name);
		free(state);
		return NULL;
	}
	return state;
}

static void output_send_enter_or_leave(
	struct np_surface *surface, struct np_output_state *state, bool entering)
{
	if (!surface || !state) return;
	struct wl_client *client = wl_resource_get_client(surface->resource);
	struct np_output *output;
	wl_list_for_each(output, &state->server->outputs, link) {
		if (output->state != state ||
		    wl_resource_get_client(output->resource) != client) continue;
		if (entering) wl_surface_send_enter(surface->resource, output->resource);
		else wl_surface_send_leave(surface->resource, output->resource);
	}
}

static void output_state_remove(struct np_output_state *state)
{
	if (!state || !state->global) return;
	struct np_surface *surface;
	wl_list_for_each(surface, &state->server->surfaces, link) {
		if (surface->output_id != state->id) continue;
		output_send_enter_or_leave(surface, state, false);
		surface->output_id = 0;
	}
	wl_global_destroy(state->global);
	state->global = NULL;
	output_state_maybe_destroy(state);
}

static void output_state_update(
	struct np_output_state *state, const struct np_host_output *value)
{
	char *name = strdup(value->name);
	if (!name) return;
	free(state->name);
	state->name = name;
	state->x = value->x;
	state->y = value->y;
	state->width = value->width;
	state->height = value->height;
	state->pixel_width = value->pixel_width;
	state->pixel_height = value->pixel_height;
	state->physical_width_mm = value->physical_width_mm;
	state->physical_height_mm = value->physical_height_mm;
	state->scale = value->scale;
	state->refresh_millihz = value->refresh_millihz;
	struct np_output *output;
	wl_list_for_each(output, &state->server->outputs, link) {
		if (output->state == state) output_send_state(state, output->resource);
	}
}

void np_scale_surface_enter_outputs(struct np_surface *surface,
	                                struct wl_client *client)
{
	if (!surface) return;
	if (!surface->output_id) {
		struct np_output_state *state = first_output_state(surface->server);
		if (state) surface->output_id = state->id;
	}
	struct np_output *output;
	wl_list_for_each(output, &surface->server->outputs, link) {
		if (output->state->id == surface->output_id &&
		    wl_resource_get_client(output->resource) == client)
			wl_surface_send_enter(surface->resource, output->resource);
	}
}

void np_scale_changed(struct np_surface *surface, int scale)
{
	if (!surface || scale <= 0) return;
	surface->preferred_scale = scale;
	send_preferred_scale(surface, scale);
}

bool np_scale_update_outputs(struct np_server *server,
	                         const struct np_host_output *values,
	                         size_t count)
{
	if (!server || (!values && count)) return false;
	for (size_t i = 0; i < count; i++) {
		const struct np_host_output *value = &values[i];
		if (!value->id || !value->name || !value->name[0] ||
		    strlen(value->name) > 255 || value->width <= 0 ||
		    value->height <= 0 || value->pixel_width <= 0 ||
		    value->pixel_height <= 0 || value->physical_width_mm < 0 ||
		    value->physical_height_mm < 0 || value->scale <= 0 ||
		    value->refresh_millihz <= 0) return false;
		for (size_t j = 0; j < i; j++)
			if (values[j].id == value->id) return false;
	}

	struct np_output_state *state, *temporary;
	wl_list_for_each_safe(state, temporary, &server->output_states, link) {
		if (!state->global) continue;
		bool present = false;
		for (size_t i = 0; i < count; i++)
			if (values[i].id == state->id) present = true;
		if (!present) output_state_remove(state);
	}
	for (size_t i = 0; i < count; i++) {
		state = output_state_by_id(server, values[i].id);
		if (state) output_state_update(state, &values[i]);
		else if (!output_state_create(server, &values[i])) return false;
	}
	state = first_output_state(server);
	if (state) {
		server->output_scale = state->scale;
		server->output_width = state->pixel_width;
		server->output_height = state->pixel_height;
		struct np_surface *surface;
		wl_list_for_each(surface, &server->surfaces, link) {
			if (surface->output_id) continue;
			surface->output_id = state->id;
			output_send_enter_or_leave(surface, state, true);
			np_scale_changed(surface, state->scale);
		}
	}
	wl_display_flush_clients(server->display);
	return true;
}

void np_scale_window_output_changed(struct np_server *server,
	                                uint32_t window_id, uint32_t output_id)
{
	struct np_surface *root = np_surface_by_window(server, window_id);
	if (!root) return;
	struct np_output_state *next = output_state_by_id(server, output_id);
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (np_scene_root(surface) != root) continue;
		struct np_output_state *previous = output_state_by_id(
			server, surface->output_id);
		if (previous == next) continue;
		output_send_enter_or_leave(surface, previous, false);
		surface->output_id = next ? next->id : 0;
		output_send_enter_or_leave(surface, next, true);
		if (next) np_scale_changed(surface, next->scale);
	}
	wl_display_flush_clients(server->display);
}

void np_scale_advertise(struct wl_display *display, struct np_server *server)
{
	wl_global_create(display, &wp_viewporter_interface, 1, server, viewporter_bind);
	wl_global_create(display, &wp_fractional_scale_manager_v1_interface, 1,
	                 server, fractional_manager_bind);
	struct np_host_output fallback = {
		.id = UINT32_MAX,
		.name = "NativePipe virtual display",
		.x = 0, .y = 0,
		.width = server->output_width / server->output_scale,
		.height = server->output_height / server->output_scale,
		.pixel_width = server->output_width,
		.pixel_height = server->output_height,
		.physical_width_mm = 345,
		.physical_height_mm = 224,
		.scale = server->output_scale,
		.refresh_millihz = 60000,
	};
	(void)output_state_create(server, &fallback);
}
