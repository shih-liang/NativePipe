#ifndef NP_SCENE_H
#define NP_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct np_surface;

struct np_scene_packet {
	unsigned char *data;
	size_t size;
};

/* Returns the xdg_toplevel/xdg_popup root which owns surface, or NULL for an
 * unroled tree such as a cursor or drag icon. */
struct np_surface *np_scene_root(struct np_surface *surface);

/* Resolve an AppKit content-view point to Wayland's top-most input surface and
 * return coordinates local to that surface. */
struct np_surface *np_scene_hit_test(struct np_surface *root, double x, double y,
	                                double *local_x, double *local_y);

/* Resolve current Wayland state into one bounded NPSN layer snapshot. Source
 * resources remain retained until np_scene_presented. */
bool np_scene_build(struct np_surface *root, uint32_t presentation_id,
	                struct np_scene_packet *packet);

/* Retain one unroled cursor/drag surface source while the host reads it. */
bool np_scene_hold_current(struct np_surface *surface, uint32_t presentation_id);

void np_scene_presented(struct np_surface *root, uint32_t presentation_id);
/* Drop every host-read hold owned by this surface. Used both at surface
 * destruction and when the host connection disappears before release events
 * can arrive. */
void np_scene_discard_presentations(struct np_surface *surface);
void np_scene_destroy(struct np_surface *surface);

#endif
