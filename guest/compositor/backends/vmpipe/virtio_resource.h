#ifndef NP_VIRTIO_RESOURCE_H
#define NP_VIRTIO_RESOURCE_H

#include <stdbool.h>
#include <stdint.h>

struct np_virtio_resource_ref {
    int drm_fd;
    uint32_t bo_handle;
    uint32_t resource_id;
};

/* Import a PRIME fd into the owning virtio-gpu render node without creating a
 * DRM/Venus context, then query the transport resource id. */
bool np_virtio_resource_import_prime(int prime_fd,
                                     struct np_virtio_resource_ref *out);
void np_virtio_resource_release(struct np_virtio_resource_ref *ref);

/* Opens the Venus-capable render node only for PRIME/resource-info lookups.
 * No DRM_IOCTL_VIRTGPU_CONTEXT_INIT is issued. */
int np_virtio_open_lookup_node(void);

#endif
