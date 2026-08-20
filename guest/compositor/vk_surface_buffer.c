#define _GNU_SOURCE

#include "vk_surface_buffer.h"

#include <string.h>
#include <stdio.h>

/*
 * The Vulkan device is intentionally owned by the compositor GPU backend.
 * This module only owns the lifetime of the shared surface allocation.
 *
 * The old implementation created a virtio blob, mmap'ed it, then copied GPU
 * images into it. This file is the replacement boundary: one VkImage and one
 * VkDeviceMemory will become both the CPU mapped surface and the GPU render
 * target.
 *
 * Device bootstrap is kept outside this file because the compositor already
 * has a Venus instance/device path. Once that path is wired in, the callbacks
 * below become the only allocation path used by Wayland surfaces.
 */

static bool np_vk_surface_buffer_zero(struct np_vk_surface_buffer *buffer)
{
    if (!buffer)
        return false;

    memset(buffer, 0, sizeof(*buffer));
    return true;
}

bool np_vk_surface_buffer_create(uint32_t width,
                                 uint32_t height,
                                 struct np_vk_surface_buffer *out)
{
    if (!out || width == 0 || height == 0)
        return false;

    np_vk_surface_buffer_zero(out);

    out->width = width;
    out->height = height;
    out->stride = width * 4;

    /*
     * Allocation is intentionally not duplicated here. The next migration
     * step wires this object to the compositor VkDevice:
     *
     *   vkCreateImage()
     *   vkAllocateMemory()
     *   vkBindImageMemory()
     *   vkMapMemory()
     *
     * Keeping the object creation boundary first avoids reintroducing the
     * previous blob/Vulkan split.
     */
    fprintf(stderr,
            "[vk-surface] allocation requested %ux%u\n",
            width, height);

    return true;
}

void np_vk_surface_buffer_destroy(struct np_vk_surface_buffer *buffer)
{
    if (!buffer)
        return;

    /* Vulkan object destruction will be added when the compositor device
     * ownership is moved here. */
    memset(buffer, 0, sizeof(*buffer));
}
