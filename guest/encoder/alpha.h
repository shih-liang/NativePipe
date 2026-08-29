#ifndef REMOTEPIPE_ALPHA_H
#define REMOTEPIPE_ALPHA_H

#include <stdbool.h>
#include <stdint.h>

/* Pack the fourth byte of each BGRA pixel with the MediaWire PackBits format.
 * The caller owns *packed on success. */
bool np_alpha_pack_bgra(
	const uint8_t *bgra, int width, int height, int stride,
	uint8_t **packed, uint32_t *packed_size);

#endif
