#define _GNU_SOURCE

/* linux-dmabuf is an input protocol only. Client resources never become
 * NativePipe display resources directly: each wl_buffer owns a compositor
 * VkImage/IOSurface mirror, and commit copies the client image into that mirror
 * before compositor.c publishes its resource id. */

#include "dmabuf.h"
#include "gpu_copy.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "vk_surface_buffer.h"

#include <drm/drm.h>
#include <drm/virtgpu_drm.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_MOD_LINEAR 0ull
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffull

struct np_dmabuf {
    /* Borrowed lookup-only render-node fd owned by the compositor surface store.
     * It has no DRM/Venus context. */
    int drm_fd;
};

struct np_params {
    int drm_fd;
    int fd;
    uint32_t stride;
    uint64_t modifier;
    bool has_plane;
};

struct np_gpu_buffer_object {
    struct np_gpu_buffer info;          /* publishes display.resource_id */
    struct np_gpu_copy_source source;   /* client dma-buf, compositor-local VkImage */
    struct np_vk_surface_buffer display;/* compositor-owned IOSurface VkImage */

    uint32_t client_bo_handle;          /* lifetime reference only */
    uint32_t client_resource_id;        /* diagnostic only; never sent to host */
    int drm_fd;                         /* borrowed lookup fd */
};

static uint32_t resource_from_prime(int drm_fd, int prime_fd, uint32_t *bo_out)
{
    struct drm_prime_handle prime;
    memset(&prime, 0, sizeof(prime));
    prime.fd = prime_fd;
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0) {
        fprintf(stderr, "[wayland] PRIME_FD_TO_HANDLE: %s\n", strerror(errno));
        return 0;
    }

    struct drm_virtgpu_resource_info info;
    memset(&info, 0, sizeof(info));
    info.bo_handle = prime.handle;
    if (ioctl(drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info) < 0 || !info.res_handle) {
        fprintf(stderr, "[wayland] RESOURCE_INFO: %s\n", strerror(errno));
        struct drm_gem_close closer = { .handle = prime.handle };
        ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &closer);
        return 0;
    }

    *bo_out = prime.handle;
    return info.res_handle;
}

static void gpu_buffer_free(struct np_gpu_buffer_object *gpu)
{
    if (!gpu)
        return;

    np_gpu_copy_source_destroy(&gpu->source);
    np_vk_surface_buffer_destroy(&gpu->display);

    if (gpu->client_bo_handle && gpu->drm_fd >= 0) {
        struct drm_gem_close closer = { .handle = gpu->client_bo_handle };
        ioctl(gpu->drm_fd, DRM_IOCTL_GEM_CLOSE, &closer);
    }
    free(gpu);
}

static void gpu_buffer_resource_destroy(struct wl_resource *resource)
{
    gpu_buffer_free(wl_resource_get_user_data(resource));
}

static void gpu_buffer_destroy_request(struct wl_client *client,
                                       struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface gpu_buffer_implementation = {
    .destroy = gpu_buffer_destroy_request,
};

static bool supported_format(uint32_t format)
{
    return format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888;
}

static struct wl_resource *make_gpu_buffer(struct wl_client *client, uint32_t id,
                                           struct np_params *params,
                                           int32_t width, int32_t height,
                                           uint32_t format)
{
    if (!params->has_plane || params->fd < 0 || width <= 0 || height <= 0 ||
        !supported_format(format))
        return NULL;

    if (params->modifier != DRM_FORMAT_MOD_LINEAR &&
        params->modifier != DRM_FORMAT_MOD_INVALID) {
        fprintf(stderr, "[wayland] unsupported dmabuf modifier 0x%llx\n",
                (unsigned long long)params->modifier);
        return NULL;
    }

    uint32_t client_bo = 0;
    uint32_t client_res = resource_from_prime(params->drm_fd, params->fd, &client_bo);
    if (!client_res)
        return NULL;

    struct np_gpu_buffer_object *gpu = calloc(1, sizeof(*gpu));
    if (!gpu) {
        struct drm_gem_close closer = { .handle = client_bo };
        ioctl(params->drm_fd, DRM_IOCTL_GEM_CLOSE, &closer);
        return NULL;
    }
    gpu->drm_fd = params->drm_fd;
    gpu->client_bo_handle = client_bo;
    gpu->client_resource_id = client_res;

    if (!np_gpu_copy_source_import(params->fd, format,
                                   (uint32_t)width, (uint32_t)height,
                                   params->stride, &gpu->source)) {
        fprintf(stderr, "[wayland] failed to import client dmabuf res=%u into Vulkan\n",
                client_res);
        gpu_buffer_free(gpu);
        return NULL;
    }

    if (!np_vk_surface_buffer_create((uint32_t)width, (uint32_t)height,
                                     &gpu->display)) {
        fprintf(stderr, "[wayland] failed to allocate display VkImage for client res=%u\n",
                client_res);
        gpu_buffer_free(gpu);
        return NULL;
    }

    gpu->info.resource_id = gpu->display.resource_id;
    gpu->info.width = width;
    gpu->info.height = height;
    gpu->info.stride = (int32_t)gpu->display.stride;
    gpu->info.format = format;

    close(params->fd);
    params->fd = -1;
    params->has_plane = false;

    struct wl_resource *buffer = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer) {
        gpu_buffer_free(gpu);
        return NULL;
    }
    wl_resource_set_implementation(buffer, &gpu_buffer_implementation, gpu,
                                   gpu_buffer_resource_destroy);

    fprintf(stderr,
            "[wayland] gpu buffer client_res=%u -> display_res=%u %dx%d stride=%u\n",
            client_res, gpu->display.resource_id, width, height, gpu->display.stride);
    return buffer;
}

static void params_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void params_add(struct wl_client *client, struct wl_resource *resource,
                       int32_t fd, uint32_t plane_idx, uint32_t offset,
                       uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo)
{
    (void)client;
    struct np_params *params = wl_resource_get_user_data(resource);

    if (plane_idx != 0) {
        close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "only plane 0 is supported");
        return;
    }
    if (params->has_plane) {
        close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane 0 already set");
        return;
    }
    if (offset != 0) {
        close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
                               "non-zero plane offsets are not supported");
        return;
    }

    params->fd = fd;
    params->stride = stride;
    params->modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    params->has_plane = true;
}

static void params_create(struct wl_client *client, struct wl_resource *resource,
                          int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
    (void)flags;
    struct np_params *params = wl_resource_get_user_data(resource);
    struct wl_resource *buffer = make_gpu_buffer(client, 0, params, width, height, format);
    if (!buffer) {
        zwp_linux_buffer_params_v1_send_failed(resource);
        return;
    }
    zwp_linux_buffer_params_v1_send_created(resource, buffer);
}

static void params_create_immed(struct wl_client *client, struct wl_resource *resource,
                                uint32_t buffer_id, int32_t width, int32_t height,
                                uint32_t format, uint32_t flags)
{
    (void)flags;
    struct np_params *params = wl_resource_get_user_data(resource);
    if (!make_gpu_buffer(client, buffer_id, params, width, height, format)) {
        wl_resource_post_error(resource,
                               ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                               "could not create NativePipe display mirror");
    }
}

static const struct zwp_linux_buffer_params_v1_interface params_implementation = {
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

static void params_resource_destroy(struct wl_resource *resource)
{
    struct np_params *params = wl_resource_get_user_data(resource);
    if (!params)
        return;
    if (params->fd >= 0)
        close(params->fd);
    free(params);
}

static void dmabuf_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void dmabuf_create_params(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t id)
{
    struct np_dmabuf *dmabuf = wl_resource_get_user_data(resource);
    struct np_params *params = calloc(1, sizeof(*params));
    if (!params) {
        wl_client_post_no_memory(client);
        return;
    }
    params->drm_fd = dmabuf->drm_fd;
    params->fd = -1;
    params->modifier = DRM_FORMAT_MOD_INVALID;

    struct wl_resource *created = wl_resource_create(
        client, &zwp_linux_buffer_params_v1_interface,
        wl_resource_get_version(resource), id);
    if (!created) {
        free(params);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(created, &params_implementation, params,
                                   params_resource_destroy);
}

static const uint32_t k_formats[] = {
    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_XRGB8888,
};
static const uint64_t k_modifiers[] = {
    DRM_FORMAT_MOD_LINEAR,
    DRM_FORMAT_MOD_INVALID,
};

/* v4 feedback is deliberately not advertised. Mesa Venus previously performed
 * a re-entrant Wayland roundtrip from CreateSwapchain on this minimal server;
 * v3 modifier events are sufficient for the linear import path used here. */
static void unsupported_feedback(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t id)
{
    (void)client; (void)id;
    wl_resource_post_error(resource, 0, "linux-dmabuf feedback requires v4");
}

static void unsupported_surface_feedback(struct wl_client *client,
                                         struct wl_resource *resource,
                                         uint32_t id, struct wl_resource *surface)
{
    (void)surface;
    unsupported_feedback(client, resource, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_implementation = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_create_params,
    .get_default_feedback = unsupported_feedback,
    .get_surface_feedback = unsupported_surface_feedback,
};

static void send_formats(struct wl_resource *resource)
{
    for (size_t i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); i++) {
        zwp_linux_dmabuf_v1_send_format(resource, k_formats[i]);
        if (wl_resource_get_version(resource) >= 3) {
            for (size_t m = 0; m < sizeof(k_modifiers) / sizeof(k_modifiers[0]); m++) {
                zwp_linux_dmabuf_v1_send_modifier(
                    resource, k_formats[i],
                    (uint32_t)(k_modifiers[m] >> 32),
                    (uint32_t)k_modifiers[m]);
            }
        }
    }
}

static void dmabuf_bind(struct wl_client *client, void *data,
                        uint32_t version, uint32_t id)
{
    struct np_dmabuf *dmabuf = data;
    if (version > 3)
        version = 3;
    struct wl_resource *resource = wl_resource_create(
        client, &zwp_linux_dmabuf_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &dmabuf_implementation, dmabuf, NULL);
    send_formats(resource);
}

void np_dmabuf_advertise(struct wl_display *display, int drm_fd)
{
    if (!display || drm_fd < 0)
        return;

    struct stat st;
    if (fstat(drm_fd, &st) < 0) {
        fprintf(stderr, "[wayland] dmabuf lookup fd invalid: %s\n", strerror(errno));
        return;
    }

    struct np_dmabuf *dmabuf = calloc(1, sizeof(*dmabuf));
    if (!dmabuf)
        return;
    dmabuf->drm_fd = drm_fd;

    wl_global_create(display, &zwp_linux_dmabuf_v1_interface, 3, dmabuf, dmabuf_bind);
    fprintf(stderr, "[wayland] linux-dmabuf v3 mirrors into compositor VkImages\n");
}

struct np_gpu_buffer *np_gpu_buffer_get(struct wl_resource *buffer)
{
    if (!buffer ||
        !wl_resource_instance_of(buffer, &wl_buffer_interface, &gpu_buffer_implementation))
        return NULL;

    struct np_gpu_buffer_object *gpu = wl_resource_get_user_data(buffer);
    if (!gpu)
        return NULL;

    if (!np_gpu_copy_to_display(&gpu->source, &gpu->display)) {
        fprintf(stderr, "[wayland] gpu copy client_res=%u -> display_res=%u failed\n",
                gpu->client_resource_id, gpu->display.resource_id);
        return NULL;
    }

    gpu->info.resource_id = gpu->display.resource_id;
    gpu->info.stride = (int32_t)gpu->display.stride;
    return &gpu->info;
}
