#include "alpha.h"

#include <stdint.h>
#include <stdlib.h>

static uint8_t alpha_at(
	const uint8_t *bgra, int stride, int width, size_t index)
{
	/* Encoder-owned frames are tightly packed. Avoid an integer division for
	 * every alpha sample on the normal path; keep arbitrary-stride support. */
	if (stride == width * 4) return bgra[index * 4 + 3];
	size_t row = index / (size_t)width;
	size_t column = index - row * (size_t)width;
	return bgra[row * (size_t)stride + column * 4 + 3];
}

bool np_alpha_pack_bgra(
	const uint8_t *bgra, int width, int height, int stride,
	uint8_t **packed, uint32_t *packed_size)
{
	if (packed) *packed = NULL;
	if (packed_size) *packed_size = 0;
	if (!bgra || !packed || !packed_size || width <= 0 || height <= 0 ||
	    width > INT32_MAX / 4 || stride < width * 4)
		return false;
	size_t count = (size_t)width * (size_t)height;
	if (height > 0 && count / (size_t)height != (size_t)width) return false;
	size_t overhead = (count + 127) / 128;
	if (count > UINT32_MAX || overhead > SIZE_MAX - count - 2) return false;
	uint8_t *output_bytes = malloc(count + overhead + 2);
	if (!output_bytes) return false;

	size_t input = 0, output = 0;
	while (input < count) {
		uint8_t value = alpha_at(bgra, stride, width, input);
		size_t run = 1;
		while (run < 128 && input + run < count &&
		       alpha_at(bgra, stride, width, input + run) == value)
			run++;
		if (run >= 3) {
			output_bytes[output++] = (uint8_t)(257 - run);
			output_bytes[output++] = value;
			input += run;
			continue;
		}

		size_t literal = input;
		input += run;
		while (input < count && input - literal < 128) {
			value = alpha_at(bgra, stride, width, input);
			run = 1;
			while (run < 128 && input + run < count &&
			       alpha_at(bgra, stride, width, input + run) == value)
				run++;
			if (run >= 3) break;
			size_t room = 128 - (input - literal);
			input += run < room ? run : room;
		}
		size_t length = input - literal;
		output_bytes[output++] = (uint8_t)(length - 1);
		for (size_t i = 0; i < length; i++)
			output_bytes[output++] = alpha_at(bgra, stride, width, literal + i);
	}
	*packed = output_bytes;
	*packed_size = (uint32_t)output;
	return true;
}
