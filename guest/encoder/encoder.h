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

typedef void (*np_encoder_output_fn)(void *user, const uint8_t *data, size_t size,
                                     uint64_t pts_ns, uint32_t resource_id,
                                     uint8_t flags,
                                     const uint8_t *alpha, uint32_t alpha_size,
                                     uint16_t bitstream_epoch,
                                     uint16_t width, uint16_t height);

struct np_encoder *np_encoder_create(int width, int height, np_encoder_output_fn out, void *user);
void np_encoder_destroy(struct np_encoder *enc);

/// Reconfigure when size changes. Bumps bitstream_epoch.
bool np_encoder_resize(struct np_encoder *enc, int width, int height);

/// Ask the next encoded frame to be an IDR (for late media-client attach).
void np_encoder_force_keyframe(struct np_encoder *enc);

/// Re-submit the newest queued/encoding/encoded image as an IDR after a host
/// reconnect. `active_resource_id` is the resource that the replay will emit.
bool np_encoder_replay_last(
    struct np_encoder *enc, uint64_t pts_ns, uint32_t replacement_resource_id,
    uint32_t *active_resource_id);

/// Upload a tightly packed BGRA8888 frame (row stride == width * 4) and encode.
bool np_encoder_push_bgra(struct np_encoder *enc, const uint8_t *bgra, int width, int height,
                          int stride, uint64_t pts_ns, uint32_t resource_id,
                          uint8_t flags);

uint16_t np_encoder_epoch(const struct np_encoder *enc);

#ifdef __cplusplus
}
#endif

#endif
