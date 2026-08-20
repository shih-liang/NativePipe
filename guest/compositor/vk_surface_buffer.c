#include "vk_surface_buffer.h"

#include <stdint.h>
#include <string.h>

static VkDevice np_device;
static VkPhysicalDevice np_physical_device;

void np_vk_surface_buffer_set_device(VkPhysicalDevice physical,
                                     VkDevice device)
{
    np_physical_device = physical;
    np_device = device;
}

static bool find_host_visible_memory(uint32_t bits,
                                     uint32_t *type)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(np_physical_device, &props);

    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if (!(bits & (1u << i)))
            continue;

        if (props.memoryTypes[i].propertyFlags &
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            *type = i;
            return true;
        }
    }

    return false;
}

bool np_vk_surface_buffer_create(uint32_t width,
                                 uint32_t height,
                                 struct np_vk_surface_buffer *out)
{
    if (!out || np_device == VK_NULL_HANDLE)
        return false;

    memset(out, 0, sizeof(*out));

    out->width = width;
    out->height = height;
    out->stride = width * 4;

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(np_device, &image_info, NULL, &out->image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(np_device, out->image, &requirements);

    uint32_t memory_type;
    if (!find_host_visible_memory(requirements.memoryTypeBits,
                                  &memory_type)) {
        vkDestroyImage(np_device, out->image, NULL);
        memset(out, 0, sizeof(*out));
        return false;
    }

    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };

    if (vkAllocateMemory(np_device, &alloc, NULL, &out->memory) != VK_SUCCESS) {
        vkDestroyImage(np_device, out->image, NULL);
        memset(out, 0, sizeof(*out));
        return false;
    }

    if (vkBindImageMemory(np_device, out->image, out->memory, 0) != VK_SUCCESS) {
        vkFreeMemory(np_device, out->memory, NULL);
        vkDestroyImage(np_device, out->image, NULL);
        memset(out, 0, sizeof(*out));
        return false;
    }

    if (vkMapMemory(np_device, out->memory, 0, VK_WHOLE_SIZE,
                    0, &out->mapped) != VK_SUCCESS) {
        vkFreeMemory(np_device, out->memory, NULL);
        vkDestroyImage(np_device, out->image, NULL);
        memset(out, 0, sizeof(*out));
        return false;
    }

    return true;
}

void np_vk_surface_buffer_destroy(struct np_vk_surface_buffer *buffer)
{
    if (!buffer || np_device == VK_NULL_HANDLE)
        return;

    if (buffer->mapped)
        vkUnmapMemory(np_device, buffer->memory);

    if (buffer->memory)
        vkFreeMemory(np_device, buffer->memory, NULL);

    if (buffer->image)
        vkDestroyImage(np_device, buffer->image, NULL);

    memset(buffer, 0, sizeof(*buffer));
}
