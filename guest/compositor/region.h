#ifndef NP_REGION_H
#define NP_REGION_H

#include <stdbool.h>
#include <stdint.h>

struct wl_client;
struct wl_resource;

struct np_region_box {
	int64_t x, y, width, height;
};

/* wl_region has no protocol-defined complexity limit. Keep an exact dynamic
 * rectangle set: silently merging or dropping fragments changes input hit
 * testing. A zero-initialized state is valid. */
struct np_region_state {
	uint32_t count;
	uint32_t capacity;
	struct np_region_box *boxes;
};

void np_region_create(struct wl_client *client, uint32_t id);
bool np_region_copy_resource(struct wl_resource *resource,
	                         struct np_region_state *destination);
void np_region_fini(struct np_region_state *region);
void np_region_move(struct np_region_state *destination,
	                struct np_region_state *source);
bool np_region_contains(const struct np_region_state *region,
	                    double x, double y);

#endif
