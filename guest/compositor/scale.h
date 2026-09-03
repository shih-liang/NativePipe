#ifndef NP_SCALE_H
#define NP_SCALE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "damage.h"

struct np_server;
struct np_surface;
struct np_viewport_state;
struct wl_client;
struct wl_display;

struct np_surface_mapping {
	double source_x_pixels;
	double source_y_pixels;
	double source_width_pixels;
	double source_height_pixels;
	double logical_width;
	double logical_height;
};

struct np_host_output {
	uint32_t id;
	const char *name;
	int32_t x, y, width, height;
	int32_t pixel_width, pixel_height;
	int32_t physical_width_mm, physical_height_mm;
	int32_t scale, refresh_millihz;
};

bool np_scale_transform_swaps_axes(int32_t transform);

enum np_scale_error {
	NP_SCALE_OK = 0,
	NP_SCALE_INVALID_SIZE,
	NP_SCALE_VIEWPORT_BAD_SIZE,
	NP_SCALE_VIEWPORT_OUT_OF_BUFFER,
};

enum np_scale_error np_scale_resolve_state(
	uint32_t buffer_width, uint32_t buffer_height, int32_t scale,
	int32_t transform, const struct np_viewport_state *viewport,
	struct np_surface_mapping *mapping);

bool np_scale_damage_to_buffer(
	uint32_t buffer_width, uint32_t buffer_height, int32_t scale,
	int32_t transform, const struct np_viewport_state *viewport,
	const struct np_box *surface_damage, struct np_box *buffer_damage);

/* Resolve wl_buffer.scale and wp_viewport exactly once, in the compositor.
 * Host code must never reinterpret either protocol. */
bool np_scale_resolve(const struct np_surface *surface,
	                  uint32_t buffer_width, uint32_t buffer_height,
	                  struct np_surface_mapping *mapping);

void np_scale_advertise(struct wl_display *display, struct np_server *server);
void np_scale_surface_enter_outputs(struct np_surface *surface,
	                                struct wl_client *client);
void np_scale_changed(struct np_surface *surface, int scale);
bool np_scale_update_outputs(struct np_server *server,
	                         const struct np_host_output *outputs,
	                         size_t count);
void np_scale_window_output_changed(struct np_server *server,
	                                uint32_t window_id, uint32_t output_id);

#endif
