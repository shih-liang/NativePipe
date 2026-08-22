#define _GNU_SOURCE

#include "gpu_copy.h"
#include "perf.h"

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
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
};

static struct np_gpu_copy_context g_copy;

static void log_bgra8_transfer_capabilities(VkPhysicalDevice physical)
{
    VkFormatProperties props;
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceFormatProperties(
        physical, VK_FORMAT_B8G8R8A8_UNORM, &props);

    const VkFormatFeatureFlags linear = props.linearTilingFeatures;
    fprintf(stderr,
            "[gpu-copy] BGRA8 linear features=0x%08x "
            "transfer-src=%d transfer-dst=%d blit-src=%d blit-dst=%d "
            "linear-filter=%d\n",
            linear,
            !!(linear & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT),
            !!(linear & VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
            !!(linear & VK_FORMAT_FEATURE_BLIT_SRC_BIT),
            !!(linear & VK_FORMAT_FEATURE_BLIT_DST_BIT),
            !!(linear & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT));

    VkImageFormatProperties image_props;
    VkResult src = vkGetPhysicalDeviceImageFormatProperties(
        physical, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0,
        &image_props);
    VkResult dst = vkGetPhysicalDeviceImageFormatProperties(
        physical, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0,
        &image_props);
    fprintf(stderr,
            "[gpu-copy] BGRA8 linear image support transfer-src=%d "
            "transfer-dst=%d\n",
            src == VK_SUCCESS, dst == VK_SUCCESS);
}

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
    log_bgra8_transfer_capabilities(g_copy.physical);
    g_copy.get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(ctx->device, "vkGetMemoryFdPropertiesKHR");
    if (!g_copy.get_memory_fd_properties) {
        fprintf(stderr, "[gpu-copy] vkGetMemoryFdPropertiesKHR is unavailable\n");
        goto fail;
    }

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_copy.queue_family,
    };
    VkResult result = vkCreateCommandPool(
        g_copy.device, &pool_info, NULL, &g_copy.pool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[gpu-copy] vkCreateCommandPool failed: %d\n", result);
        goto fail;
    }

    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_copy.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    result = vkAllocateCommandBuffers(
        g_copy.device, &command_info, &g_copy.command);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[gpu-copy] vkAllocateCommandBuffers failed: %d\n", result);
        goto fail;
    }

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    result = vkCreateFence(g_copy.device, &fence_info, NULL, &g_copy.fence);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[gpu-copy] vkCreateFence failed: %d\n", result);
        goto fail;
    }

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
    if (source->view)
        vkDestroyImageView(g_copy.device, source->view, NULL);
    if (source->image)
        vkDestroyImage(g_copy.device, source->image, NULL);
    /* VkDeviceMemory must outlive every image bound to it.  This path is also
     * used when a dmabuf client disappears without orderly Vulkan teardown. */
    if (source->memory)
        vkFreeMemory(g_copy.device, source->memory, NULL);
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
    if (g_copy.get_memory_fd_properties(
            g_copy.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
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
        /* The single-surface path copies from this image, while the scene
         * renderer samples the same imported image for alpha/subsurface
         * composition.  Creating an image view therefore requires SAMPLED. */
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        /* External-memory images are required to start in UNDEFINED.  The
         * dma-buf already contains client pixels; the first barrier below
         * acquires that payload for the compositor queue. */
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(g_copy.device, &image_info, NULL, &out->image) != VK_SUCCESS) {
        close(import_fd);
        return false;
    }

    VkMemoryDedicatedRequirements dedicated_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated_requirements,
    };
    VkImageMemoryRequirementsInfo2 requirements_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = out->image,
    };
    vkGetImageMemoryRequirements2(
        g_copy.device, &requirements_info, &requirements);
    const VkMemoryRequirements req = requirements.memoryRequirements;
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
    /* Venus maps the guest dma-buf to an external MTLTexture.  Metal texture
     * imports are dedicated-only, so associate the allocation with the exact
     * image even when an older guest capability query fails to expose the
     * requirement. */
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .image = out->image,
    };
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
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

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    if (vkCreateImageView(g_copy.device, &view_info, NULL, &out->view) != VK_SUCCESS) {
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
    out->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

bool np_gpu_copy_to_display_region(struct np_gpu_copy_source *source,
                                   struct np_vk_surface_buffer *display,
                                   int32_t source_x0, int32_t source_y0,
                                   int32_t source_x1, int32_t source_y1,
                                   int32_t destination_x0, int32_t destination_y0,
                                   int32_t destination_x1, int32_t destination_y1,
                                   bool clear)
{
	uint64_t perf_start = np_perf_now_ns();
    if (!source || !display || !source->image || !display->image ||
        source_x0 < 0 || source_y0 < 0 ||
        source_x1 <= source_x0 || source_y1 <= source_y0 ||
        (uint32_t)source_x1 > source->width ||
        (uint32_t)source_y1 > source->height ||
        destination_x0 < 0 || destination_y0 < 0 ||
        destination_x1 <= destination_x0 || destination_y1 <= destination_y0 ||
        (uint32_t)destination_x1 > display->width ||
        (uint32_t)destination_y1 > display->height ||
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
        /* The producer is a different Venus context and implicit dma-buf
         * synchronization has already completed in wait_for_client_render().
         * VK_ACCESS_MEMORY_WRITE_BIT would expand to HOST_WRITE as well, which
         * is not supported by ALL_COMMANDS and is not the producer here. */
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = source->layout,
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
        /* WindowServer consumption and ring-slot availability are external to
         * this Vulkan queue.  Previous compositor submits are fence-complete
         * before this command buffer is reset. */
        .srcAccessMask = display->host_dirty
            ? VK_ACCESS_HOST_WRITE_BIT : VK_ACCESS_MEMORY_WRITE_BIT,
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
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT |
                         VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 2, barriers);

    VkImageSubresourceLayers subresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .layerCount = 1,
    };
    if (clear) {
        VkClearColorValue transparent = { .float32 = {0, 0, 0, 0} };
        VkImageSubresourceRange range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        };
        vkCmdClearColorImage(g_copy.command, display->image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &transparent, 1, &range);
    }

    int32_t source_width = source_x1 - source_x0;
    int32_t source_height = source_y1 - source_y0;
    int32_t destination_width = destination_x1 - destination_x0;
    int32_t destination_height = destination_y1 - destination_y0;
    if (source_width == destination_width && source_height == destination_height) {
        VkImageCopy copy = {
            .srcSubresource = subresource,
            .srcOffset = { source_x0, source_y0, 0 },
            .dstSubresource = subresource,
            .dstOffset = { destination_x0, destination_y0, 0 },
            .extent = { (uint32_t)source_width, (uint32_t)source_height, 1 },
        };
        vkCmdCopyImage(g_copy.command,
                       source->image, VK_IMAGE_LAYOUT_GENERAL,
                       display->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &copy);
    } else {
        VkImageBlit blit = {
            .srcSubresource = subresource,
            .srcOffsets = {
                { source_x0, source_y0, 0 },
                { source_x1, source_y1, 1 },
            },
            .dstSubresource = subresource,
            .dstOffsets = {
                { destination_x0, destination_y0, 0 },
                { destination_x1, destination_y1, 1 },
            },
        };
        vkCmdBlitImage(g_copy.command,
                       source->image, VK_IMAGE_LAYOUT_GENERAL,
                       display->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
    }

    VkImageMemoryBarrier present = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        /* The next consumer is Core Animation, outside Vulkan.  Queue/fence
         * completion supplies the hand-off; no Vulkan access mask describes
         * that consumer. */
        .dstAccessMask = 0,
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
        vkWaitForFences(g_copy.device, 1, &g_copy.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
		np_perf_record(NP_PERF_GPU_COPY, np_perf_now_ns() - perf_start);
        return false;
	}
	np_perf_record(NP_PERF_GPU_COPY, np_perf_now_ns() - perf_start);

    source->initialized = true;
    source->layout = VK_IMAGE_LAYOUT_GENERAL;
    display->layout = VK_IMAGE_LAYOUT_GENERAL;
    display->host_dirty = false;
    return true;
}

bool np_gpu_copy_to_display(struct np_gpu_copy_source *source,
                            struct np_vk_surface_buffer *display)
{
    if (!source || !display || source->width > display->width ||
        source->height > display->height)
        return false;
    return np_gpu_copy_to_display_region(
        source, display, 0, 0, (int32_t)source->width, (int32_t)source->height,
        0, 0, (int32_t)source->width, (int32_t)source->height, false);
}

bool np_gpu_prepare_host_write(struct np_vk_surface_buffer *image)
{
    if (!image || !image->image || !g_copy.device)
        return false;
    if (image->layout == VK_IMAGE_LAYOUT_GENERAL && image->host_dirty)
        return true;
    if (vkWaitForFences(g_copy.device, 1, &g_copy.fence,
                        VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
        vkResetFences(g_copy.device, 1, &g_copy.fence) != VK_SUCCESS ||
        vkResetCommandBuffer(g_copy.command, 0) != VK_SUCCESS)
        return false;

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(g_copy.command, &begin) != VK_SUCCESS)
        return false;
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = image->layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0 : VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT,
        .oldLayout = image->layout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(g_copy.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    if (vkEndCommandBuffer(g_copy.command) != VK_SUCCESS)
        return false;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &g_copy.command,
    };
    if (vkQueueSubmit(g_copy.queue, 1, &submit, g_copy.fence) != VK_SUCCESS ||
        vkWaitForFences(g_copy.device, 1, &g_copy.fence,
                        VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return false;
    image->layout = VK_IMAGE_LAYOUT_GENERAL;
    if (!np_vk_surface_buffer_invalidate(image))
        return false;
    image->host_dirty = true;
    return true;
}
