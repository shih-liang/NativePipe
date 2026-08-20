#ifndef NP_GPU_COPY_H
#define NP_GPU_COPY_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "vk_context.h"
#include "vk_surface_buffer.h"

struct np_gpu_copy_source {
    VkImage image;
    VkDeviceMemory memory;
    VkFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    bool initialized;
};

bool np_gpu_copy_init(const struct np_vk_context *ctx);
void np_gpu_copy_finish(void);

/* Imports one linux-dmabuf into the compositor's Venus VkDevice. fd is borrowed;
 * the implementation duplicates it before Vulkan takes ownership. */
bool np_gpu_copy_source_import(int fd, uint32_t drm_format,
                               uint32_t width, uint32_t height,
                               uint32_t stride,
                               struct np_gpu_copy_source *out);
void np_gpu_copy_source_destroy(struct np_gpu_copy_source *source);

/* Synchronous MVP copy. Implicit dma-buf synchronization supplies producer
 * completion; the fence guarantees the display IOSurface is complete before
 * the existing host frame message is emitted. */
bool np_gpu_copy_to_display(struct np_gpu_copy_source *source,
                            struct np_vk_surface_buffer *display);

#endif
