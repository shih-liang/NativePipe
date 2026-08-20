#include "compositor.h"
#include "gpu_copy.h"
#include "vk_context.h"
#include "vk_surface_buffer.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    struct np_vk_context vk;
    if (!np_vk_context_init(&vk)) {
        fprintf(stderr, "[wayland] failed to initialize compositor Venus device\n");
        return 1;
    }

    np_vk_surface_buffer_set_device(vk.physical_device, vk.device);
    if (!np_gpu_copy_init(&vk)) {
        fprintf(stderr, "[wayland] failed to initialize compositor GPU copy path\n");
        np_vk_surface_buffer_clear_device();
        np_vk_context_destroy(&vk);
        return 1;
    }

    int result = np_compositor_run(argc, argv);
    np_gpu_copy_finish();
    np_vk_surface_buffer_clear_device();
    np_vk_context_destroy(&vk);
    return result;
}
