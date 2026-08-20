#ifndef NATIVEPIPE_BLOB_H
#define NATIVEPIPE_BLOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NP_APERTURE_ALIGNMENT ((size_t)16384)
#define NP_ROW_ALIGNMENT ((size_t)128)

static inline size_t np_align_row(size_t stride) {
    return (stride + NP_ROW_ALIGNMENT - 1) & ~(NP_ROW_ALIGNMENT - 1);
}

#ifdef NP_REMOTE

/* Remote compositor never allocates local presentation buffers, but
 * compositor.c keeps the fields in its shared surface struct. */
struct np_blob {
    uint32_t bo_handle;
    uint32_t resource_id;
    size_t size;
    void *data;
};

#else

#include "vk_surface_buffer.h"

/* Compatibility façade for compositor.c while the Wayland state machine is
 * being split into smaller modules. There is no blob allocator and no
 * CONTEXT_INIT behind these names anymore: every window allocation is a
 * compositor-owned VkImage/VkDeviceMemory. */
struct np_blob {
    uint32_t bo_handle; /* always zero; retained only for source compatibility */
    uint32_t resource_id;
    size_t size;
    void *data;
    struct np_vk_surface_buffer vk;
};

int np_surface_store_open(void);
void np_surface_store_close(int fd);
bool np_surface_store_create(int lookup_fd, size_t size,
                             uint32_t width, uint32_t height,
                             uint32_t stride, struct np_blob *out);
void np_surface_store_destroy(int lookup_fd, struct np_blob *buffer);

/* Existing Wayland code compiles unchanged, but the old symbols/routes no
 * longer exist in the binary. */
#define np_blob_open np_surface_store_open
#define np_blob_close np_surface_store_close
#define np_blob_create np_surface_store_create
#define np_blob_destroy np_surface_store_destroy

#endif

#endif
