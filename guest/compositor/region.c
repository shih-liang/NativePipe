#include "region.h"

#include <stdint.h>
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

static bool reserve_boxes(struct np_region_state *state, uint32_t needed)
{
	if (needed <= state->capacity) return true;
	uint32_t capacity = state->capacity ? state->capacity : 8u;
	while (capacity < needed) {
		if (capacity > UINT32_MAX / 2u) {
			capacity = needed;
			break;
		}
		capacity *= 2u;
	}
	if (sizeof(*state->boxes) > SIZE_MAX / (size_t)capacity) return false;
	struct np_region_box *boxes = realloc(
		state->boxes, (size_t)capacity * sizeof(*state->boxes));
	if (!boxes) return false;
	state->boxes = boxes;
	state->capacity = capacity;
	return true;
}

static bool append_piece(struct np_region_state *state,
	                     int64_t x, int64_t y, int64_t width, int64_t height)
{
	if (width <= 0 || height <= 0) return true;
	if (state->count == UINT32_MAX || !reserve_boxes(state, state->count + 1u))
		return false;
	state->boxes[state->count++] = (struct np_region_box){ x, y, width, height };
	return true;
}

void np_region_fini(struct np_region_state *region)
{
	if (!region) return;
	free(region->boxes);
	memset(region, 0, sizeof(*region));
}

void np_region_move(struct np_region_state *destination,
	                struct np_region_state *source)
{
	if (!destination || !source || destination == source) return;
	np_region_fini(destination);
	*destination = *source;
	memset(source, 0, sizeof(*source));
}

static void region_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void region_resource_destroy(struct wl_resource *resource)
{
	struct np_region *region = wl_resource_get_user_data(resource);
	if (!region) return;
	np_region_fini(&region->state);
	free(region);
}

static void region_add(struct wl_client *client, struct wl_resource *resource,
	                   int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct np_region *region = wl_resource_get_user_data(resource);
	if (!region || !valid_box(width, height)) return;
	if (!append_piece(&region->state, x, y, width, height))
		wl_client_post_no_memory(client);
}

static void region_subtract(struct wl_client *client, struct wl_resource *resource,
	                        int32_t x, int32_t y, int32_t width, int32_t height)
{
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
			if (!append_piece(&result, bx0, by0, bx1 - bx0, by1 - by0))
				goto no_memory;
			continue;
		}
		if (!append_piece(&result, bx0, by0, bx1 - bx0, iy0 - by0) ||
		    !append_piece(&result, bx0, iy1, bx1 - bx0, by1 - iy1) ||
		    !append_piece(&result, bx0, iy0, ix0 - bx0, iy1 - iy0) ||
		    !append_piece(&result, ix1, iy0, bx1 - ix1, iy1 - iy0))
			goto no_memory;
	}
	np_region_move(&region->state, &result);
	return;

no_memory:
	np_region_fini(&result);
	wl_client_post_no_memory(client);
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
	struct np_region_state copy = {0};
	if (region->state.count) {
		if (!reserve_boxes(&copy, region->state.count)) return false;
		memcpy(copy.boxes, region->state.boxes,
		       (size_t)region->state.count * sizeof(*copy.boxes));
		copy.count = region->state.count;
	}
	np_region_move(destination, &copy);
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
