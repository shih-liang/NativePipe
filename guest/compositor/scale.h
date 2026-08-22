#ifndef NP_SCALE_H
#define NP_SCALE_H

#include <stdbool.h>
#include <stdint.h>

struct np_server;
struct np_surface;
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

/* Resolve wl_buffer.scale and wp_viewport exactly once, in the compositor.
 * Host code must never reinterpret either protocol. */
bool np_scale_resolve(const struct np_surface *surface,
	                  uint32_t buffer_width, uint32_t buffer_height,
	                  struct np_surface_mapping *mapping);

void np_scale_advertise(struct wl_display *display, struct np_server *server);
void np_scale_surface_enter_outputs(struct np_surface *surface,
	                                struct wl_client *client);
void np_scale_changed(struct np_surface *surface, int scale);
void np_scale_update_output(struct np_server *server, int scale);

#endif
