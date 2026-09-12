#ifndef NATIVEPIPE_ENCODER_H
#define NATIVEPIPE_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct np_encoder;

#define NP_ENCODER_FLAG_HAS_ALPHA 1
#define NP_ENCODER_FLAG_REUSE_ALPHA 2
#define NP_ENCODER_FLAG_HARDWARE 4
enum np_encoder_codec { NP_ENCODER_H264 = 1, NP_ENCODER_AV1 = 3 };

typedef void (*np_encoder_output_fn)(void *user, const uint8_t *data, size_t size,
                                     uint64_t pts_ns, uint32_t resource_id,
                                     uint8_t flags,
                                     const uint8_t *alpha, uint32_t alpha_size,
                                     uint16_t bitstream_epoch,
                                     uint16_t width, uint16_t height,
                                     enum np_encoder_codec codec);

typedef void (*np_encoder_done_fn)(void *user, bool ok);
struct np_encoder *np_encoder_create(int width, int height, bool allow_h264, np_encoder_output_fn out,
                                    np_encoder_done_fn done, void *user);
void np_encoder_destroy(struct np_encoder *enc);

/// Upload a tightly packed BGRA8888 frame (row stride == width * 4) and encode.
bool np_encoder_push_bgra(struct np_encoder *enc, const uint8_t *bgra, int width, int height,
                          int stride, uint64_t pts_ns, uint32_t resource_id,
                          uint8_t flags);

/// Accept malloc-owned BGRA, already padded to even dimensions. Ownership moves
/// only on success; worker frees it before done(). Codec setup also runs there.
bool np_encoder_take_bgra(struct np_encoder *enc, uint8_t *pixels, int width, int height,
                         int stride, uint64_t pts_ns, uint32_t resource_id, uint8_t flags);

uint16_t np_encoder_epoch(const struct np_encoder *enc);

#ifdef __cplusplus
}
#endif

#endif
