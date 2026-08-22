#ifndef NP_GPU_COPY_H
#define NP_GPU_COPY_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "vk_context.h"
#include "vk_surface_buffer.h"

struct np_gpu_copy_source {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkImageLayout layout;
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

/* Makes a linear snapshot legal for mapped CPU writes before wl_shm copies. */
bool np_gpu_prepare_host_write(struct np_vk_surface_buffer *image);

/* Synchronous copy. Implicit dma-buf synchronization supplies producer
 * completion; the fence guarantees the compositor scene image is complete
 * before the host frame message is emitted. */
bool np_gpu_copy_to_display(struct np_gpu_copy_source *source,
                            struct np_vk_surface_buffer *display);
bool np_gpu_copy_to_display_region(struct np_gpu_copy_source *source,
                                   struct np_vk_surface_buffer *display,
                                   int32_t source_x0, int32_t source_y0,
                                   int32_t source_x1, int32_t source_y1,
                                   int32_t destination_x0, int32_t destination_y0,
                                   int32_t destination_x1, int32_t destination_y1,
                                   bool clear);

#endif
