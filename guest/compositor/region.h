#ifndef NP_REGION_H
#define NP_REGION_H

#include <stdbool.h>
#include <stdint.h>

#define NP_REGION_MAX_BOXES 64u

struct wl_client;
struct wl_resource;

struct np_region_box {
	int32_t x, y, width, height;
};

/* wl_region is mutable, but wl_surface.set_*_region copies its value into
 * double-buffered surface state. A bounded rectangle set keeps request memory
 * under compositor control and is sufficient for toolkit input/opaque regions. */
struct np_region_state {
	uint32_t count;
	struct np_region_box boxes[NP_REGION_MAX_BOXES];
};

void np_region_create(struct wl_client *client, uint32_t id);
bool np_region_copy_resource(struct wl_resource *resource,
	                         struct np_region_state *destination);
bool np_region_contains(const struct np_region_state *region,
	                    double x, double y);

#endif
