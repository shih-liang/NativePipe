#ifndef NATIVEPIPE_ENCODER_VAAPI_H
#define NATIVEPIPE_ENCODER_VAAPI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct np_av1_vaapi;
bool np_av1_vaapi_supports_size(int width, int height);
struct np_av1_vaapi *np_av1_vaapi_create(int width, int height);
/* Returns one malloc-owned low-overhead AV1 temporal unit after GPU completion. */
bool np_av1_vaapi_encode(struct np_av1_vaapi *, const uint8_t *planes[3],
    const int strides[3], bool keyframe, uint8_t **packet, size_t *size);
void np_av1_vaapi_destroy(struct np_av1_vaapi *);
#endif
