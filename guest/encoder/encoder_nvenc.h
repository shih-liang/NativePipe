#ifndef NATIVEPIPE_ENCODER_NVENC_H
#define NATIVEPIPE_ENCODER_NVENC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct np_h264_nvenc;
bool np_h264_nvenc_supports_size(int width, int height);
struct np_h264_nvenc *np_h264_nvenc_create(int width, int height);
/* One BGRA input -> one malloc-owned Annex-B access unit. All GPU reads and
 * bitstream writes finish before returning. No FFmpeg/CUDA toolkit dependency. */
bool np_h264_nvenc_encode(struct np_h264_nvenc *, const uint8_t *bgra, int stride,
    bool keyframe, uint8_t **packet, size_t *size);
void np_h264_nvenc_destroy(struct np_h264_nvenc *);
#endif
