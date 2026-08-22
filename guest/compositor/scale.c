#include "scale.h"
#include "compositor_internal.h"
#include "fractional-scale-v1-server-protocol.h"
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

bool np_scale_resolve(const struct np_surface *surface,
	                  uint32_t buffer_width, uint32_t buffer_height,
	                  struct np_surface_mapping *mapping)
{
	if (!surface || !mapping || !buffer_width || !buffer_height || surface->scale <= 0)
		return false;

	double scale = surface->scale;
	double transformed_width = np_scale_transform_swaps_axes(surface->transform)
		? buffer_height : buffer_width;
	double transformed_height = np_scale_transform_swaps_axes(surface->transform)
		? buffer_width : buffer_height;
	double logical_source_x = 0;
	double logical_source_y = 0;
	double logical_source_width = transformed_width / scale;
	double logical_source_height = transformed_height / scale;
	if (surface->viewport_state.source_set) {
		logical_source_x = wl_fixed_to_double(surface->viewport_state.source_x);
		logical_source_y = wl_fixed_to_double(surface->viewport_state.source_y);
		logical_source_width = wl_fixed_to_double(surface->viewport_state.source_width);
		logical_source_height = wl_fixed_to_double(surface->viewport_state.source_height);
	}
	if (logical_source_x < 0 || logical_source_y < 0 ||
	    logical_source_width <= 0 || logical_source_height <= 0 ||
	    (logical_source_x + logical_source_width) * scale > transformed_width + 0.001 ||
	    (logical_source_y + logical_source_height) * scale > transformed_height + 0.001)
		return false;

	transformed_rect_to_source(
		surface->transform, transformed_width, transformed_height,
		buffer_width, buffer_height,
		logical_source_x * scale, logical_source_y * scale,
		logical_source_width * scale, logical_source_height * scale,
		&mapping->source_x_pixels, &mapping->source_y_pixels,
		&mapping->source_width_pixels, &mapping->source_height_pixels);
	if (surface->viewport_state.destination_set) {
		mapping->logical_width = surface->viewport_state.destination_width;
		mapping->logical_height = surface->viewport_state.destination_height;
	} else {
		mapping->logical_width = logical_source_width;
		mapping->logical_height = logical_source_height;
	}
	return mapping->logical_width > 0 && mapping->logical_height > 0;
}

bool np_scale_damage_to_buffer(int32_t transform,
	                           uint32_t buffer_width, uint32_t buffer_height,
	                           int32_t scale, int32_t x, int32_t y,
	                           int32_t width, int32_t height,
	                           int32_t *buffer_x, int32_t *buffer_y,
	                           int32_t *buffer_width_out,
	                           int32_t *buffer_height_out)
{
	if (!buffer_width || !buffer_height || scale <= 0 || width <= 0 || height <= 0 ||
	    !buffer_x || !buffer_y || !buffer_width_out || !buffer_height_out)
		return false;
	double transformed_width = np_scale_transform_swaps_axes(transform)
		? buffer_height : buffer_width;
	double transformed_height = np_scale_transform_swaps_axes(transform)
		? buffer_width : buffer_height;
	double bx, by, bw, bh;
	transformed_rect_to_source(
		transform, transformed_width, transformed_height,
		buffer_width, buffer_height,
		(double)x * scale, (double)y * scale,
		(double)width * scale, (double)height * scale,
		&bx, &by, &bw, &bh);
	*buffer_x = (int32_t)floor(bx + 0.0001);
	*buffer_y = (int32_t)floor(by + 0.0001);
	*buffer_width_out = (int32_t)ceil(bx + bw - *buffer_x - 0.0001);
	*buffer_height_out = (int32_t)ceil(by + bh - *buffer_y - 0.0001);
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
	send_preferred_scale(surface, surface->server->output_scale);
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

static void output_resource_destroy(struct wl_resource *resource)
{
	struct np_output *output = wl_resource_get_user_data(resource);
	if (!output)
		return;
	wl_list_remove(&output->link);
	free(output);
}

static void output_send_state(struct np_server *server, struct wl_resource *resource)
{
	uint32_t version = (uint32_t)wl_resource_get_version(resource);
	if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
		wl_output_send_scale(resource, server->output_scale);
	if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
		wl_output_send_done(resource);
}

static void output_bind(struct wl_client *client, void *data,
	                   uint32_t version, uint32_t id)
{
	struct np_server *server = data;
	struct wl_resource *resource = wl_resource_create(
		client, &wl_output_interface, (int)version, id);
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
	wl_output_send_geometry(resource, 0, 0, 345, 224, WL_OUTPUT_SUBPIXEL_UNKNOWN,
	                        "Apple", "NativePipe", WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
	                    server->output_width, server->output_height, 60000);
	output_send_state(server, resource);

	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (wl_resource_get_client(surface->resource) == client)
			wl_surface_send_enter(surface->resource, resource);
	}
}

void np_scale_surface_enter_outputs(struct np_surface *surface,
	                                struct wl_client *client)
{
	struct np_output *output;
	wl_list_for_each(output, &surface->server->outputs, link) {
		if (wl_resource_get_client(output->resource) == client)
			wl_surface_send_enter(surface->resource, output->resource);
	}
}

void np_scale_changed(struct np_surface *surface, int scale)
{
	send_preferred_scale(surface, scale);
}

void np_scale_update_output(struct np_server *server, int scale)
{
	if (!server || scale <= 0 || scale == server->output_scale)
		return;
	server->output_scale = scale;
	struct np_output *output;
	wl_list_for_each(output, &server->outputs, link)
		output_send_state(server, output->resource);
}

void np_scale_advertise(struct wl_display *display, struct np_server *server)
{
	wl_global_create(display, &wl_output_interface, 2, server, output_bind);
	wl_global_create(display, &wp_viewporter_interface, 1, server, viewporter_bind);
	wl_global_create(display, &wp_fractional_scale_manager_v1_interface, 1,
	                 server, fractional_manager_bind);
}
