#ifndef NP_VK_SURFACE_BUFFER_H
#define NP_VK_SURFACE_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

/* One compositor-owned image. CPU wl_shm writes and GPU copy operations target
 * the same VkImage/VkDeviceMemory; the exported Venus renderer BO supplies the
 * virtio resource id that the host presents as an IOSurface. */
struct np_vk_surface_buffer {
    VkImage image;
    VkDeviceMemory memory;
    VkImageLayout layout;

    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint32_t stride;

    void *mapped;
    size_t size;
    bool coherent;

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
void np_vk_surface_buffer_destroy(struct np_vk_surface_buffer *buffer);

#endif
