#ifndef NP_GPU_BLIT_H
#define NP_GPU_BLIT_H

#include <stdbool.h>
#include <stdint.h>

#include "damage.h"

// Guest-side GPU blit for compositor IOSurface window buffers.
//
// Compositor window storage is VkCreateImage + vkAllocateMemory (iosurface_allowed
// ctx) exported via RESOURCE_CREATE_BLOB. Client dmabuf images are copied with
// vkCmdCopyImage; the host presents the blob directly (presentCPU).

#include "blob.h"
#include "dmabuf.h"

bool np_gpu_window_buffer_create(int drm_fd, size_t size, uint32_t width, uint32_t height,
                                 uint32_t stride, struct np_blob *out);
void np_gpu_window_buffer_destroy(int drm_fd, struct np_blob *blob);

bool np_gpu_blit_init(int drm_fd);
void np_gpu_blit_fini(void);

/// GPU-copy damaged regions from `src` into compositor blob `dst`.
bool np_gpu_blit_into_blob(struct np_gpu_buffer *src, struct np_blob *dst,
                           int32_t width, int32_t height, uint32_t dst_stride,
                           const struct np_damage_region *damage);

/// Drop cached VkImage bindings after blob destroy/replace.
void np_gpu_blit_invalidate_blob(uint32_t resource_id);

#endif
