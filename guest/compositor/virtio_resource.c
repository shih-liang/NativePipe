#define _GNU_SOURCE

#include "virtio_resource.h"

#include <drm/drm.h>
#include <drm/virtgpu_drm.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define NP_VENUS_CAPSET_ID 4u
#define NP_FIRST_RENDER_NODE 128
#define NP_LAST_RENDER_NODE 191

static bool node_has_venus_capset(int fd)
{
    unsigned char caps[4096];
    memset(caps, 0, sizeof(caps));

    struct drm_virtgpu_get_caps args;
    memset(&args, 0, sizeof(args));
    args.cap_set_id = NP_VENUS_CAPSET_ID;
    args.cap_set_ver = 0;
    args.addr = (uint64_t)(uintptr_t)caps;
    args.size = sizeof(caps);

    return ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &args) == 0;
}

int np_virtio_open_lookup_node(void)
{
    for (int minor = NP_FIRST_RENDER_NODE; minor <= NP_LAST_RENDER_NODE; minor++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (node_has_venus_capset(fd)) {
            fprintf(stderr, "[virtio] Venus lookup node %s (no DRM context)\n", path);
            return fd;
        }
        close(fd);
    }

    errno = ENODEV;
    return -1;
}

static bool try_import_on_fd(int drm_fd, int prime_fd,
                             struct np_virtio_resource_ref *out)
{
    struct drm_prime_handle prime;
    memset(&prime, 0, sizeof(prime));
    prime.fd = prime_fd;
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0)
        return false;

    struct drm_virtgpu_resource_info info;
    memset(&info, 0, sizeof(info));
    info.bo_handle = prime.handle;
    if (ioctl(drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info) < 0 ||
        info.res_handle == 0) {
        struct drm_gem_close close_req = { .handle = prime.handle };
        ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_req);
        return false;
    }

    out->drm_fd = drm_fd;
    out->bo_handle = prime.handle;
    out->resource_id = info.res_handle;
    return true;
}

bool np_virtio_resource_import_prime(int prime_fd,
                                     struct np_virtio_resource_ref *out)
{
    if (!out || prime_fd < 0)
        return false;

    memset(out, 0, sizeof(*out));
    out->drm_fd = -1;

    /* Try only the Venus-capable node. Importing a dma-buf does not require a
     * rendering context; the GEM handle is kept only as a lifetime reference. */
    int fd = np_virtio_open_lookup_node();
    if (fd < 0)
        return false;

    if (!try_import_on_fd(fd, prime_fd, out)) {
        close(fd);
        out->drm_fd = -1;
        return false;
    }

    return true;
}

void np_virtio_resource_release(struct np_virtio_resource_ref *ref)
{
    if (!ref)
        return;

    if (ref->drm_fd >= 0 && ref->bo_handle) {
        struct drm_gem_close close_req = { .handle = ref->bo_handle };
        ioctl(ref->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_req);
    }
    if (ref->drm_fd >= 0)
        close(ref->drm_fd);

    memset(ref, 0, sizeof(*ref));
    ref->drm_fd = -1;
}
