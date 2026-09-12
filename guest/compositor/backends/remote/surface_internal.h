#ifndef NP_REMOTE_SURFACE_INTERNAL_H
#define NP_REMOTE_SURFACE_INTERNAL_H

#include "../../../encoder/encoder.h"

#include <stdbool.h>
#include <stdint.h>

struct np_remote_surface {
	struct np_encoder *encoder;
	struct np_remote_scene_job *job;
	uint16_t last_epoch;
	unsigned char *pixels;
	uint64_t pts_ns;
	uint8_t flags;
    struct { uint32_t id; bool scene; } flight[8];
    unsigned flight_count;
    uint64_t next_refresh_ns;
    uint32_t display_interval_ns;
};

struct np_surface;

#endif
