#ifndef NP_SCENE_H
#define NP_SCENE_H

#include <stdbool.h>
#include <stdint.h>

struct np_surface;

struct np_scene_frame {
	uint32_t resource_id;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t scale;
	int32_t geometry_x;
	int32_t geometry_y;
	int32_t geometry_width;
	int32_t geometry_height;
	int slot;
	bool gpu;
};

/* Returns the xdg_toplevel/xdg_popup root which owns surface, or NULL for an
 * unroled tree such as a cursor or drag icon. */
struct np_surface *np_scene_root(struct np_surface *surface);

/* Atomically resolves and composites the root's current Wayland surface tree
 * into the sole compositor output image. The host copies it into an
 * NSWindow-owned IOSurface and then releases this frame. */
bool np_scene_compose(struct np_surface *root, uint32_t presentation_id,
	                  struct np_scene_frame *frame);

void np_scene_presented(struct np_surface *root, uint32_t presentation_id);
void np_scene_destroy(struct np_surface *surface);

#endif
