#include "region.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-protocol.h>

struct np_region {
	struct np_region_state state;
};

static bool valid_box(int32_t width, int32_t height)
{
	return width > 0 && height > 0;
}

static void region_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void region_resource_destroy(struct wl_resource *resource)
{
	free(wl_resource_get_user_data(resource));
}

static void region_add(struct wl_client *client, struct wl_resource *resource,
	                   int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client;
	struct np_region *region = wl_resource_get_user_data(resource);
	if (!region || !valid_box(width, height)) return;
	if (region->state.count == NP_REGION_MAX_BOXES) {
		/* Preserve a conservative input superset instead of allocating from an
		 * untrusted request stream. Merge the new box into the last entry. */
		struct np_region_box *box = &region->state.boxes[NP_REGION_MAX_BOXES - 1];
		int64_t left = box->x < x ? box->x : x;
		int64_t top = box->y < y ? box->y : y;
		int64_t right0 = (int64_t)box->x + box->width;
		int64_t right1 = (int64_t)x + width;
		int64_t bottom0 = (int64_t)box->y + box->height;
		int64_t bottom1 = (int64_t)y + height;
		int64_t right = right0 > right1 ? right0 : right1;
		int64_t bottom = bottom0 > bottom1 ? bottom0 : bottom1;
		box->x = (int32_t)(left < INT32_MIN ? INT32_MIN : left);
		box->y = (int32_t)(top < INT32_MIN ? INT32_MIN : top);
		box->width = (int32_t)(right - box->x > INT32_MAX ? INT32_MAX : right - box->x);
		box->height = (int32_t)(bottom - box->y > INT32_MAX ? INT32_MAX : bottom - box->y);
		return;
	}
	region->state.boxes[region->state.count++] =
		(struct np_region_box){ x, y, width, height };
}

static void append_piece(struct np_region_state *state,
	                     int64_t x, int64_t y, int64_t width, int64_t height)
{
	if (width <= 0 || height <= 0 || state->count == NP_REGION_MAX_BOXES) return;
	state->boxes[state->count++] = (struct np_region_box){
		.x = (int32_t)x, .y = (int32_t)y,
		.width = (int32_t)width, .height = (int32_t)height,
	};
}

static void region_subtract(struct wl_client *client, struct wl_resource *resource,
	                        int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client;
	struct np_region *region = wl_resource_get_user_data(resource);
	if (!region || !valid_box(width, height)) return;
	struct np_region_state result = {0};
	int64_t sx0 = x, sy0 = y, sx1 = sx0 + width, sy1 = sy0 + height;
	for (uint32_t i = 0; i < region->state.count; i++) {
		const struct np_region_box *box = &region->state.boxes[i];
		int64_t bx0 = box->x, by0 = box->y;
		int64_t bx1 = bx0 + box->width, by1 = by0 + box->height;
		int64_t ix0 = bx0 > sx0 ? bx0 : sx0;
		int64_t iy0 = by0 > sy0 ? by0 : sy0;
		int64_t ix1 = bx1 < sx1 ? bx1 : sx1;
		int64_t iy1 = by1 < sy1 ? by1 : sy1;
		if (ix0 >= ix1 || iy0 >= iy1) {
			append_piece(&result, bx0, by0, bx1 - bx0, by1 - by0);
			continue;
		}
		append_piece(&result, bx0, by0, bx1 - bx0, iy0 - by0);
		append_piece(&result, bx0, iy1, bx1 - bx0, by1 - iy1);
		append_piece(&result, bx0, iy0, ix0 - bx0, iy1 - iy0);
		append_piece(&result, ix1, iy0, bx1 - ix1, iy1 - iy0);
	}
	region->state = result;
}

static const struct wl_region_interface region_implementation = {
	.destroy = region_destroy,
	.add = region_add,
	.subtract = region_subtract,
};

void np_region_create(struct wl_client *client, uint32_t id)
{
	struct np_region *region = calloc(1, sizeof(*region));
	if (!region) {
		wl_client_post_no_memory(client);
		return;
	}
	struct wl_resource *resource = wl_resource_create(client, &wl_region_interface, 1, id);
	if (!resource) {
		free(region);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &region_implementation, region,
	                               region_resource_destroy);
}

bool np_region_copy_resource(struct wl_resource *resource,
	                         struct np_region_state *destination)
{
	if (!resource || !destination ||
	    !wl_resource_instance_of(resource, &wl_region_interface,
	                             &region_implementation))
		return false;
	struct np_region *region = wl_resource_get_user_data(resource);
	if (!region) return false;
	*destination = region->state;
	return true;
}

bool np_region_contains(const struct np_region_state *region, double x, double y)
{
	if (!region) return false;
	for (uint32_t i = 0; i < region->count; i++) {
		const struct np_region_box *box = &region->boxes[i];
		if (x >= box->x && y >= box->y &&
		    x < (double)box->x + box->width &&
		    y < (double)box->y + box->height)
			return true;
	}
	return false;
}
