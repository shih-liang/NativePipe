#ifndef NATIVEPIPE_AV1_DECODER_H
#define NATIVEPIPE_AV1_DECODER_H
#include <CoreVideo/CoreVideo.h>
#include <stddef.h>
#include <stdint.h>
struct np_av1_decoder;
struct np_av1_decoder *np_av1_decoder_create(void);
void np_av1_decoder_destroy(struct np_av1_decoder *);
/* One low-delay temporal unit -> a new, independently owned BGRA IOSurface. */
CVPixelBufferRef np_av1_decoder_decode(struct np_av1_decoder *, const uint8_t *, size_t,
    int width, int height) CF_RETURNS_RETAINED;
/* Validates the negotiated 8-bit 4:2:0 profile and writes the four av1C bytes. */
int np_av1_configuration(const uint8_t *, size_t, int width, int height, uint8_t config[4]);
#endif
