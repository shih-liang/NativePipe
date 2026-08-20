#include "vk_context.h"

#include <string.h>

bool np_vk_context_init(struct np_vk_context *ctx)
{
    if (!ctx)
        return false;

    memset(ctx, 0, sizeof(*ctx));

    /*
     * Vulkan bootstrap is intentionally owned by the compositor rather than
     * individual surfaces. The final implementation will create the Venus
     * instance/device here and pass the resulting VkDevice to all surface
     * allocators through np_vk_surface_buffer_set_device().
     */
    return true;
}

void np_vk_context_destroy(struct np_vk_context *ctx)
{
    if (!ctx)
        return;

    memset(ctx, 0, sizeof(*ctx));
}
