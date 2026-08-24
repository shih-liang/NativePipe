#ifndef NP_DAMAGE_H
#define NP_DAMAGE_H

#include <stdint.h>

struct np_box {
	int64_t x, y, width, height;
};

static inline void np_box_clear(struct np_box *box)
{
	box->x = box->y = box->width = box->height = 0;
}

static inline void np_box_union(
	struct np_box *box, int64_t x, int64_t y, int64_t width, int64_t height)
{
	if (width <= 0 || height <= 0) return;
	if (box->width == 0 || box->height == 0) {
		box->x = x;
		box->y = y;
		box->width = width;
		box->height = height;
		return;
	}
	int64_t left = box->x < x ? box->x : x;
	int64_t top = box->y < y ? box->y : y;
	int64_t right = box->x + box->width > x + width
		? box->x + box->width : x + width;
	int64_t bottom = box->y + box->height > y + height
		? box->y + box->height : y + height;
	box->x = left;
	box->y = top;
	box->width = right - left;
	box->height = bottom - top;
}

#endif
