#ifndef NP_DAMAGE_H
#define NP_DAMAGE_H

#include <stdint.h>

struct np_box {
	int64_t x, y, width, height;
};

struct np_damage_region {
	struct np_box *rects;
	uint32_t count;
	uint32_t capacity;
	bool overflow;
};

#endif
