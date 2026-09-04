#ifndef NP_REMOTE_SURFACE_INTERNAL_H
#define NP_REMOTE_SURFACE_INTERNAL_H

#include "../../../encoder/encoder.h"

#include <stdbool.h>
#include <stdint.h>

struct np_remote_surface {
	struct np_encoder *encoder;
	uint16_t last_epoch;
};

struct np_surface;

bool np_remote_republish_surface(struct np_surface *surface);
void np_remote_force_keyframe(struct np_surface *surface);

#endif
