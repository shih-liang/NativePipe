#define _GNU_SOURCE

#include "gpu_copy.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u

struct np_gpu_copy_context {
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
};

static struct np_gpu_copy_context g_copy;

static VkFormat drm_format_to_vk(uint32_t format)
{
    switch (format) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

bool np_gpu_copy_init(const struct np_vk_context *ctx)
{
    if (!ctx || !ctx->device || !ctx->graphics_queue)
        return false;
    memset(&g_copy, 0, sizeof(g_copy));
    g_copy.physical = ctx->physical_device;
    g_copy.device = ctx->device;
    g_copy.queue = ctx->graphics_queue;
    g_copy.queue_family = ctx->graphics_queue_family;

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_copy.queue_family,
    };
    if (vkCreateCommandPool(g_copy.device, &pool_info, NULL, &g_copy.pool) != VK_SUCCESS)
        goto fail;

    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_copy.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(g_copy.device, &command_info, &g_copy.command) != VK_SUCCESS)
        goto fail;

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    if (vkCreateFence(g_copy.device, &fence_info, NULL, &g_copy.fence) != VK_SUCCESS)
        goto fail;

    return true;

fail:
    np_gpu_copy_finish();
    return false;
}

void np_gpu_copy_finish(void)
{
    if (g_copy.device && g_copy.fence)
        vkDestroyFence(g_copy.device, g_copy.fence, NULL);
    if (g_copy.device && g_copy.pool)
        vkDestroyCommandPool(g_copy.device, g_copy.pool, NULL);
    memset(&g_copy, 0, sizeof(g_copy));
}

static bool pick_memory_type(uint32_t bits, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_copy.physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if (bits & (1u << i)) {
            *index = i;
            return true;
        }
    }
    return false;
}

void np_gpu_copy_source_destroy(struct np_gpu_copy_source *source)
{
    if (!source || !g_copy.device)
        return;
    if (source->memory)
        vkFreeMemory(g_copy.device, source->memory, NULL);
    if (source->image)
        vkDestroyImage(g_copy.device, source->image, NULL);
    memset(source, 0, sizeof(*source));
}

bool np_gpu_copy_source_import(int fd, uint32_t drm_format,
                               uint32_t width, uint32_t height,
                               uint32_t stride,
                               struct np_gpu_copy_source *out)
{
    if (!out || fd < 0 || !width || !height || !g_copy.device)
        return false;
    memset(out, 0, sizeof(*out));

    VkFormat format = drm_format_to_vk(drm_format);
    if (format == VK_FORMAT_UNDEFINED)
        return false;

    int import_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (import_fd < 0)
        return false;

    VkMemoryFdPropertiesKHR fd_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    if (vkGetMemoryFdPropertiesKHR(g_copy.device,
                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                   import_fd, &fd_props) != VK_SUCCESS) {
        close(import_fd);
        return false;
    }

    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
    };
    if (vkCreateImage(g_copy.device, &image_info, NULL, &out->image) != VK_SUCCESS) {
        close(import_fd);
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_copy.device, out->image, &req);
    uint32_t memory_type = 0;
    if (!pick_memory_type(req.memoryTypeBits & fd_props.memoryTypeBits, &memory_type)) {
        close(import_fd);
        np_gpu_copy_source_destroy(out);
        return false;
    }

    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = import_fd,
    };
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = req.size,
        .memoryTypeIndex = memory_type,
    };
    VkResult result = vkAllocateMemory(g_copy.device, &alloc, NULL, &out->memory);
    if (result != VK_SUCCESS) {
        close(import_fd); /* Vulkan takes ownership only on successful import. */
        np_gpu_copy_source_destroy(out);
        return false;
    }

    if (vkBindImageMemory(g_copy.device, out->image, out->memory, 0) != VK_SUCCESS) {
        np_gpu_copy_source_destroy(out);
        return false;
    }

    VkImageSubresource subresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .arrayLayer = 0,
    };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(g_copy.device, out->image, &subresource, &layout);
    if (layout.rowPitch != stride) {
        fprintf(stderr, "[gpu-copy] dmabuf stride=%u Vulkan rowPitch=%llu\n",
                stride, (unsigned long long)layout.rowPitch);
        np_gpu_copy_source_destroy(out);
        return false;
    }

    out->format = format;
    out->width = width;
    out->height = height;
    out->stride = stride;
    return true;
}

bool np_gpu_copy_to_display(struct np_gpu_copy_source *source,
                            struct np_vk_surface_buffer *display)
{
    if (!source || !display || !source->image || !display->image ||
        source->width != display->width || source->height != display->height ||
        !g_copy.device)
        return false;

    if (vkWaitForFences(g_copy.device, 1, &g_copy.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return false;
    if (vkResetFences(g_copy.device, 1, &g_copy.fence) != VK_SUCCESS ||
        vkResetCommandBuffer(g_copy.command, 0) != VK_SUCCESS)
        return false;

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(g_copy.command, &begin) != VK_SUCCESS)
        return false;

    VkImageMemoryBarrier src_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = source->initialized ? VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = source->initialized ? VK_IMAGE_LAYOUT_GENERAL
                                         : VK_IMAGE_LAYOUT_PREINITIALIZED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = source->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkImageMemoryBarrier dst_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = display->layout == VK_IMAGE_LAYOUT_GENERAL
                             ? VK_ACCESS_MEMORY_READ_BIT : 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = display->layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = display->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkImageMemoryBarrier barriers[] = { src_barrier, dst_barrier };
    vkCmdPipelineBarrier(g_copy.command,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 2, barriers);

    VkImageCopy copy = {
        .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .extent = { source->width, source->height, 1 },
    };
    vkCmdCopyImage(g_copy.command,
                   source->image, VK_IMAGE_LAYOUT_GENERAL,
                   display->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &copy);

    VkImageMemoryBarrier present = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = display->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(g_copy.command,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &present);

    if (vkEndCommandBuffer(g_copy.command) != VK_SUCCESS)
        return false;

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &g_copy.command,
    };
    if (vkQueueSubmit(g_copy.queue, 1, &submit, g_copy.fence) != VK_SUCCESS ||
        vkWaitForFences(g_copy.device, 1, &g_copy.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return false;

    source->initialized = true;
    display->layout = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}
