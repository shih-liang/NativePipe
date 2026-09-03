#ifndef NP_VK_CONTEXT_H
#define NP_VK_CONTEXT_H

#include <stdbool.h>
#include <vulkan/vulkan.h>

struct np_vk_context {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    uint32_t graphics_queue_family;
};

bool np_vk_context_init(struct np_vk_context *ctx);
void np_vk_context_destroy(struct np_vk_context *ctx);

#endif
