#include "blob.h"
#include "virtio_resource.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int np_surface_store_open(void)
{
    /* This fd exists only for linux-dmabuf PRIME/resource-info lookup. It has no
     * Venus context and never submits rendering commands. */
    return np_virtio_open_lookup_node();
}

void np_surface_store_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

bool np_surface_store_create(int lookup_fd, size_t requested_size,
                             uint32_t width, uint32_t height,
                             uint32_t requested_stride, struct np_blob *out)
{
    (void)lookup_fd;
    (void)requested_size;

    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    if (!np_vk_surface_buffer_create(width, height, &out->vk))
        return false;

    /* compositor.c currently computes the host IOSurface row alignment before
     * allocation. Keep that contract strict until publish_frame is moved into
     * this module; silently using two different row pitches corrupts rows. */
    if (requested_stride != out->vk.stride) {
        fprintf(stderr,
                "[surface-store] Vulkan rowPitch=%u differs from compositor stride=%u\n",
                out->vk.stride, requested_stride);
        np_vk_surface_buffer_destroy(&out->vk);
        memset(out, 0, sizeof(*out));
        return false;
    }

    out->bo_handle = 0;
    out->resource_id = out->vk.resource_id;
    out->size = out->vk.size;
    out->data = out->vk.mapped;

    fprintf(stderr, "[surface-store] VkImage res=%u %ux%u stride=%u\n",
            out->resource_id, width, height, out->vk.stride);
    return true;
}

void np_surface_store_destroy(int lookup_fd, struct np_blob *buffer)
{
    (void)lookup_fd;
    if (!buffer)
        return;

    np_vk_surface_buffer_destroy(&buffer->vk);
    memset(buffer, 0, sizeof(*buffer));
}
