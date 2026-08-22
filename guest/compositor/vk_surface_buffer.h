#ifndef NP_VK_SURFACE_BUFFER_H
#define NP_VK_SURFACE_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

/* One compositor-owned wl_shm upload image. Guest CPU writes the mapped linear
 * VkImage; the host samples its existing MoltenVK texture as one scene layer. */
struct np_vk_surface_buffer {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkImageLayout layout;

    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint32_t stride;

    void *mapped;
    size_t size;
    bool coherent;
    /* True after the mapped CPU view was written and flushed.  Vulkan must
     * consume that write through a HOST_WRITE barrier before loading or
     * overwriting the image.  Cleared after a GPU submission writes it. */
    bool host_dirty;

    /* Context-free PRIME import kept alive while the frame can be presented. */
    int resource_drm_fd;
    uint32_t resource_bo_handle;
};

/* The compositor owns exactly one Venus VkDevice and installs it before any
 * surface allocation. Surface code never creates a Vulkan instance/context. */
void np_vk_surface_buffer_set_device(VkPhysicalDevice physical, VkDevice device);
void np_vk_surface_buffer_clear_device(void);

bool np_vk_surface_buffer_create(uint32_t width,
                                 uint32_t height,
                                 struct np_vk_surface_buffer *out);
void np_vk_surface_buffer_flush(struct np_vk_surface_buffer *buffer);
bool np_vk_surface_buffer_invalidate(struct np_vk_surface_buffer *buffer);
void np_vk_surface_buffer_destroy(struct np_vk_surface_buffer *buffer);

#endif
