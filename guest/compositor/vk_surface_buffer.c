#include "vk_surface_buffer.h"
#include "virtio_resource.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

static VkDevice np_device;
static VkPhysicalDevice np_physical_device;
static PFN_vkGetMemoryFdKHR np_get_memory_fd;

void np_vk_surface_buffer_set_device(VkPhysicalDevice physical,
                                     VkDevice device)
{
    np_physical_device = physical;
    np_device = device;
    np_get_memory_fd = (PFN_vkGetMemoryFdKHR)
        vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
}

void np_vk_surface_buffer_clear_device(void)
{
    np_device = VK_NULL_HANDLE;
    np_physical_device = VK_NULL_HANDLE;
    np_get_memory_fd = NULL;
}

static bool find_host_visible_memory(uint32_t bits,
                                     uint32_t *type,
                                     bool *coherent)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(np_physical_device, &props);

    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
            if (!(bits & (1u << i)))
                continue;

            VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
            if (!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                continue;
            if (pass == 0 && !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                continue;

            *type = i;
            *coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            return true;
        }
    }

    return false;
}

static void destroy_partial(struct np_vk_surface_buffer *buffer)
{
    if (!buffer)
        return;

    if (buffer->resource_drm_fd >= 0 &&
        (buffer->resource_bo_handle || buffer->resource_id)) {
        struct np_virtio_resource_ref ref = {
            .drm_fd = buffer->resource_drm_fd,
            .bo_handle = buffer->resource_bo_handle,
            .resource_id = buffer->resource_id,
        };
        np_virtio_resource_release(&ref);
    }

    if (np_device != VK_NULL_HANDLE && buffer->view)
        vkDestroyImageView(np_device, buffer->view, NULL);
    if (np_device != VK_NULL_HANDLE && buffer->image)
        vkDestroyImage(np_device, buffer->image, NULL);
    if (np_device != VK_NULL_HANDLE && buffer->mapped && buffer->memory)
        vkUnmapMemory(np_device, buffer->memory);
    /* Destroy the bound image before releasing its VkDeviceMemory. */
    if (np_device != VK_NULL_HANDLE && buffer->memory)
        vkFreeMemory(np_device, buffer->memory, NULL);

    memset(buffer, 0, sizeof(*buffer));
    buffer->resource_drm_fd = -1;
}

bool np_vk_surface_buffer_create(uint32_t width,
                                 uint32_t height,
                                 struct np_vk_surface_buffer *out)
{
    if (!out || !width || !height ||
        np_device == VK_NULL_HANDLE || np_physical_device == VK_NULL_HANDLE ||
        !np_get_memory_fd)
        return false;

    memset(out, 0, sizeof(*out));
    out->resource_drm_fd = -1;
    out->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    out->width = width;
    out->height = height;

    VkExternalMemoryImageCreateInfo external_image = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        /* Surface snapshots are sampled and the two window images are render
         * targets. MoltenVK on Apple GPUs advertises renderLinearTextures, and
         * the compositor validates these linear-format features at startup. */
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(np_device, &image_info, NULL, &out->image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(np_device, out->image, &requirements);
    out->size = (size_t)requirements.size;

    uint32_t memory_type = 0;
    if (!find_host_visible_memory(requirements.memoryTypeBits,
                                  &memory_type,
                                  &out->coherent)) {
        destroy_partial(out);
        return false;
    }

    VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &export_info,
        .image = out->image,
        .buffer = VK_NULL_HANDLE,
    };
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };

    if (vkAllocateMemory(np_device, &alloc, NULL, &out->memory) != VK_SUCCESS) {
        destroy_partial(out);
        return false;
    }

    if (vkBindImageMemory(np_device, out->image, out->memory, 0) != VK_SUCCESS) {
        destroy_partial(out);
        return false;
    }

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    if (vkCreateImageView(np_device, &view_info, NULL, &out->view) != VK_SUCCESS) {
        destroy_partial(out);
        return false;
    }

    void *base = NULL;
    if (vkMapMemory(np_device, out->memory, 0, VK_WHOLE_SIZE, 0, &base) != VK_SUCCESS) {
        destroy_partial(out);
        return false;
    }

    VkImageSubresource subresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .arrayLayer = 0,
    };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(np_device, out->image, &subresource, &layout);
    if (!layout.rowPitch || layout.rowPitch > UINT32_MAX ||
        layout.offset >= requirements.size) {
        out->mapped = base;
        destroy_partial(out);
        return false;
    }

    out->mapped = (unsigned char *)base + layout.offset;
    out->stride = (uint32_t)layout.rowPitch;

    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = out->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int prime_fd = -1;
    if (np_get_memory_fd(np_device, &fd_info, &prime_fd) != VK_SUCCESS || prime_fd < 0) {
        destroy_partial(out);
        return false;
    }

    struct np_virtio_resource_ref ref;
    bool imported = np_virtio_resource_import_prime(prime_fd, &ref);
    close(prime_fd);
    if (!imported) {
        destroy_partial(out);
        return false;
    }

    out->resource_id = ref.resource_id;
    out->resource_drm_fd = ref.drm_fd;
    out->resource_bo_handle = ref.bo_handle;
    return true;
}

void np_vk_surface_buffer_flush(struct np_vk_surface_buffer *buffer)
{
    if (!buffer || !buffer->memory || np_device == VK_NULL_HANDLE)
        return;

    buffer->host_dirty = true;
    if (buffer->coherent)
        return;

    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = buffer->memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkFlushMappedMemoryRanges(np_device, 1, &range);
}

bool np_vk_surface_buffer_invalidate(struct np_vk_surface_buffer *buffer)
{
    if (!buffer || !buffer->memory || np_device == VK_NULL_HANDLE)
        return false;
    if (buffer->coherent)
        return true;

    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = buffer->memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    return vkInvalidateMappedMemoryRanges(np_device, 1, &range) == VK_SUCCESS;
}

void np_vk_surface_buffer_destroy(struct np_vk_surface_buffer *buffer)
{
    if (!buffer)
        return;
    destroy_partial(buffer);
}
