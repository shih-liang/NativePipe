#ifndef NP_VK_SURFACE_BUFFER_H
#define NP_VK_SURFACE_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

/*
 * Unified compositor buffer.
 *
 * The same VkImage/VkDeviceMemory is used for:
 *   - CPU access through vkMapMemory
 *   - GPU rendering through Vulkan commands
 *   - host presentation through the exported virtio resource
 *
 * This replaces the old split model of blob mmap + GPU copy.
 */
struct np_vk_surface_buffer {
    VkImage image;
    VkDeviceMemory memory;

    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint32_t stride;

    void *mapped;
    size_t size;
};

/*
 * The compositor owns Vulkan initialization. Surface allocation receives the
 * already-created Venus VkPhysicalDevice/VkDevice rather than creating another
 * Vulkan context.
 */
void np_vk_surface_buffer_set_device(VkPhysicalDevice physical,
                                     VkDevice device);

bool np_vk_surface_buffer_create(uint32_t width,
                                 uint32_t height,
                                 struct np_vk_surface_buffer *out);

void np_vk_surface_buffer_destroy(struct np_vk_surface_buffer *buffer);

#endif
