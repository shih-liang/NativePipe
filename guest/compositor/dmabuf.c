#define _GNU_SOURCE

/* linux-dmabuf is an input protocol only. Its virtio resource already names
 * the host MTLTexture, so importing it into a second guest VkImage would add a
 * copy and an unnecessary Vulkan lifetime. */

#include "dmabuf.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "perf.h"
#include "syncobj.h"

#include <drm/drm.h>
#include <drm/virtgpu_drm.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_MOD_LINEAR 0ull
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffull
#define DRM_FORMAT_MOD_APPLE_GPU_TILED 0x0c00000000000001ull

struct np_dmabuf {
    /* Borrowed lookup-only render-node fd owned by the compositor surface store.
     * It has no DRM/Venus context. */
    int drm_fd;
    dev_t device;
};

struct np_dmabuf_format_table_entry {
    uint32_t format;
    uint32_t padding;
    uint64_t modifier;
};

struct np_params {
    int drm_fd;
    int fd;
	uint32_t offset;
    uint32_t stride;
    uint64_t modifier;
    bool has_plane;
	bool used;
};

struct np_gpu_buffer_object {
    struct np_gpu_buffer info;          /* identifies the client's host texture */

    uint32_t client_bo_handle;          /* lifetime reference only */
    uint32_t client_resource_id;        /* diagnostic only; never sent to host */
    int drm_fd;                         /* borrowed lookup fd */
	int dma_buf_fd;                    /* owned, also exports a pollable fence */
	struct wl_client *client;          /* alive whenever a release is queued */
	struct wl_resource *resource;
	uint32_t references;
	uint32_t current_references;
	uint32_t host_reads;
	bool release_pending;
	struct wl_list sync_releases;
};

struct np_gpu_sync_release {
	struct wl_list link;
	struct np_sync_point *point;
};

static struct np_gpu_buffer_object *gpu_object(struct np_gpu_buffer *buffer)
{
	return buffer ? (struct np_gpu_buffer_object *)buffer : NULL;
}

static void gpu_buffer_ref(struct np_gpu_buffer_object *gpu)
{
	if (gpu) gpu->references++;
}

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
	if (!gpu) return;

	struct np_gpu_sync_release *release, *tmp;
	wl_list_for_each_safe(release, tmp, &gpu->sync_releases, link) {
		wl_list_remove(&release->link);
		np_sync_point_signal(release->point);
		free(release);
	}
	if (gpu->client_bo_handle && gpu->drm_fd >= 0) {
		struct drm_gem_close closer = { .handle = gpu->client_bo_handle };
		ioctl(gpu->drm_fd, DRM_IOCTL_GEM_CLOSE, &closer);
	}
	if (gpu->dma_buf_fd >= 0) close(gpu->dma_buf_fd);
	free(gpu);
}

static void gpu_buffer_unref(struct np_gpu_buffer_object *gpu)
{
	if (!gpu || !gpu->references || --gpu->references) return;
	gpu_buffer_free(gpu);
}

void np_gpu_buffer_retain(struct np_gpu_buffer *buffer)
{
	gpu_buffer_ref(gpu_object(buffer));
}

void np_gpu_buffer_drop(struct np_gpu_buffer *buffer)
{
	gpu_buffer_unref(gpu_object(buffer));
}

static void maybe_release_client(struct np_gpu_buffer_object *gpu)
{
	if (!gpu || gpu->current_references || gpu->host_reads) return;
	struct np_gpu_sync_release *release, *tmp;
	wl_list_for_each_safe(release, tmp, &gpu->sync_releases, link) {
		wl_list_remove(&release->link);
		np_sync_point_signal(release->point);
		free(release);
	}
	if (gpu->release_pending && gpu->resource) {
		gpu->release_pending = false;
		wl_buffer_send_release(gpu->resource);
	}
}

static enum np_gpu_read_result wait_for_client_render(
	struct np_gpu_buffer_object *gpu, int *wait_fd)
{
	if (wait_fd) *wait_fd = -1;
	uint64_t start = np_perf_now_ns();
	struct drm_virtgpu_3d_wait wait = {
		.handle = gpu->client_bo_handle,
		/* Never stall the Wayland event loop behind application rendering.
		 * The exported dma-resv sync_file wakes the loop when the producer is
		 * actually ready. */
		.flags = VIRTGPU_WAIT_NOWAIT,
	};
	if (ioctl(gpu->drm_fd, DRM_IOCTL_VIRTGPU_WAIT, &wait) == 0) {
		np_perf_record(NP_PERF_CLIENT_WAIT, np_perf_now_ns() - start);
		return NP_GPU_READ_READY;
	}
	np_perf_record(NP_PERF_CLIENT_WAIT, np_perf_now_ns() - start);

	if (errno == EBUSY || errno == EAGAIN) {
		struct dma_buf_export_sync_file export = {
			.flags = DMA_BUF_SYNC_READ,
			.fd = -1,
		};
		if (gpu->dma_buf_fd >= 0 &&
		    ioctl(gpu->dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export) == 0 &&
		    export.fd >= 0) {
			fcntl(export.fd, F_SETFD, FD_CLOEXEC);
			if (wait_fd) *wait_fd = export.fd;
			else close(export.fd);
		}
		return NP_GPU_READ_WAIT;
	}

	fprintf(stderr, "[wayland] WAIT client_res=%u: %s\n",
	        gpu->client_resource_id, strerror(errno));
	return NP_GPU_READ_FAILED;
}

static void gpu_buffer_resource_destroy(struct wl_resource *resource)
{
	struct np_gpu_buffer_object *gpu = wl_resource_get_user_data(resource);
	if (!gpu) return;
	gpu->resource = NULL;
	gpu_buffer_unref(gpu); /* protocol ownership */
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
	    params->offset != 0 || !supported_format(format))
        return NULL;

    if (params->modifier != DRM_FORMAT_MOD_INVALID &&
        params->modifier != DRM_FORMAT_MOD_LINEAR &&
        params->modifier != DRM_FORMAT_MOD_APPLE_GPU_TILED) {
        fprintf(stderr, "[wayland] unsupported dmabuf modifier 0x%llx\n",
                (unsigned long long)params->modifier);
        return NULL;
    }

    uint32_t client_bo = 0;
	uint32_t client_res = resource_from_prime(params->drm_fd, params->fd, &client_bo);
	if (!client_res) return NULL;

	struct np_gpu_buffer_object *gpu = calloc(1, sizeof(*gpu));
    if (!gpu) {
        struct drm_gem_close closer = { .handle = client_bo };
        ioctl(params->drm_fd, DRM_IOCTL_GEM_CLOSE, &closer);
        return NULL;
    }
	gpu->drm_fd = params->drm_fd;
	gpu->dma_buf_fd = params->fd;
	gpu->client = client;
    gpu->client_bo_handle = client_bo;
    gpu->client_resource_id = client_res;
	gpu->references = 1;
	wl_list_init(&gpu->sync_releases);

    gpu->info.resource_id = client_res;
    gpu->info.width = width;
    gpu->info.height = height;
    gpu->info.stride = (int32_t)params->stride;
    gpu->info.format = format;

	params->fd = -1;
    params->has_plane = false;

    struct wl_resource *buffer = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer) {
        gpu_buffer_free(gpu);
        return NULL;
    }
    wl_resource_set_implementation(buffer, &gpu_buffer_implementation, gpu,
                                   gpu_buffer_resource_destroy);
	gpu->resource = buffer;

    if (getenv("NP_TRACE")) {
        fprintf(stderr,
                "[wayland] gpu source client_res=%u %dx%d stride=%u\n",
                client_res, width, height, params->stride);
    }
    return buffer;
}

void np_gpu_buffer_acquire_current(struct np_gpu_buffer *buffer)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (!gpu) return;
	gpu->current_references++;
	gpu->release_pending = false;
	gpu_buffer_ref(gpu);
}

void np_gpu_buffer_release_current(struct np_gpu_buffer *buffer)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (!gpu || !gpu->current_references) return;
	gpu->current_references--;
	gpu->release_pending = true;
	maybe_release_client(gpu);
	gpu_buffer_unref(gpu);
}

bool np_gpu_buffer_acquire_host_read(struct np_gpu_buffer *buffer)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (!gpu) return false;
	gpu->host_reads++;
	gpu_buffer_ref(gpu);
	return true;
}

enum np_gpu_read_result np_gpu_buffer_render_status(
	struct np_gpu_buffer *buffer, int *wait_fd)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (!gpu) {
		if (wait_fd) *wait_fd = -1;
		return NP_GPU_READ_FAILED;
	}
	return wait_for_client_render(gpu, wait_fd);
}

void np_gpu_buffer_end_host_read(struct np_gpu_buffer *buffer)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (!gpu || !gpu->host_reads) return;
	gpu->host_reads--;
	maybe_release_client(gpu);
	gpu_buffer_unref(gpu);
}

bool np_gpu_buffer_is_busy(struct np_gpu_buffer *buffer)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	return gpu && gpu->host_reads != 0;
}

void np_gpu_buffer_queue_release(
	struct np_gpu_buffer *buffer, struct np_sync_point *point)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (!point) return;
	if (!gpu) {
		np_sync_point_signal(point);
		return;
	}
	struct np_gpu_sync_release *release = calloc(1, sizeof(*release));
	if (!release) {
		/* Never signal while Metal may still be reading. Disconnecting the
		 * client releases its syncobj state instead of leaving a live client
		 * blocked forever on an unsignalled point. */
		fprintf(stderr, "[wayland] could not retain explicit release point\n");
		np_sync_point_destroy(point);
		wl_client_post_no_memory(gpu->client);
		return;
	}
	release->point = point;
	wl_list_insert(gpu->sync_releases.prev, &release->link);
	maybe_release_client(gpu);
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
	if (params->used) {
		close(fd);
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
			"buffer params have already been used");
		return;
	}

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
	params->fd = fd;
	params->offset = offset;
    params->stride = stride;
    params->modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    params->has_plane = true;
}

static bool params_validate_create(
	struct wl_resource *resource, struct np_params *params,
	int32_t width, int32_t height, uint32_t format)
{
	if (params->used) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
			"buffer params have already been used");
		return false;
	}
	params->used = true;
	if (!params->has_plane || params->fd < 0) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			"ARGB/XRGB buffers require exactly one plane");
		return false;
	}
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
			"buffer dimensions must be positive");
		return false;
	}
	if (!supported_format(format) ||
	    (params->modifier != DRM_FORMAT_MOD_INVALID &&
	     params->modifier != DRM_FORMAT_MOD_LINEAR &&
	     params->modifier != DRM_FORMAT_MOD_APPLE_GPU_TILED)) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			"unsupported format or modifier");
		return false;
	}
	uint64_t row_bytes = (uint64_t)(uint32_t)width * 4u;
	if (params->modifier == DRM_FORMAT_MOD_APPLE_GPU_TILED) {
		/* drm_fourcc.h deliberately defines a synthetic width*cpp pitch for
		 * Apple tiled images.  The exact VkImage/MTLTexture owns the real layout. */
		if (params->offset != 0 || params->stride != row_bytes) {
			wl_resource_post_error(resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
				"invalid Apple GPU tiled offset or stride convention");
			return false;
		}
		return true;
	}
	uint64_t last_row = (uint64_t)(uint32_t)(height - 1) * params->stride;
	uint64_t required = (uint64_t)params->offset + last_row + row_bytes;
	if (params->stride < row_bytes || required < last_row || required < row_bytes) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			"invalid plane stride or range");
		return false;
	}
	struct stat st;
	off_t size = fstat(params->fd, &st) == 0 ? st.st_size : -1;
	if (size == 0) size = lseek(params->fd, 0, SEEK_END);
	if (size <= 0 || required > (uint64_t)size) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			"plane extends outside the dma-buf");
		return false;
	}
	return true;
}

static void params_create(struct wl_client *client, struct wl_resource *resource,
                          int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
	struct np_params *params = wl_resource_get_user_data(resource);
	if (!params_validate_create(resource, params, width, height, format)) return;
	if (flags != 0 || params->offset != 0) {
		zwp_linux_buffer_params_v1_send_failed(resource);
		return;
	}
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
	struct np_params *params = wl_resource_get_user_data(resource);
	if (!params_validate_create(resource, params, width, height, format)) return;
	if (flags != 0 || params->offset != 0) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
			"unsupported dma-buf flags or non-zero plane offset");
		return;
	}
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
    /* NativePipe's private Apple-family token maps to optimal tiling in VGL. */
    DRM_FORMAT_MOD_APPLE_GPU_TILED,
    DRM_FORMAT_MOD_LINEAR,
};

/* Classic VirGL does not implement pipe_screen::resource_create_with_modifiers.
 * Mesa therefore requires INVALID in dmabuf feedback before it will allocate
 * an implicit-modifier window buffer.  Keep INVALID out of the legacy
 * `modifier` events: the legacy `format` event already advertises that path. */
static const uint64_t k_feedback_modifiers[] = {
    DRM_FORMAT_MOD_APPLE_GPU_TILED,
    DRM_FORMAT_MOD_LINEAR,
    DRM_FORMAT_MOD_INVALID,
};

static void feedback_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_implementation = {
    .destroy = feedback_destroy,
};

static bool write_all(int fd, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size) {
        ssize_t written = write(fd, bytes, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        bytes += written;
        size -= (size_t)written;
    }
    return true;
}

static int create_format_table(void)
{
    struct np_dmabuf_format_table_entry entries[
        sizeof(k_formats) / sizeof(k_formats[0]) *
        sizeof(k_feedback_modifiers) / sizeof(k_feedback_modifiers[0])];
    size_t entry = 0;
    for (size_t f = 0; f < sizeof(k_formats) / sizeof(k_formats[0]); f++) {
        for (size_t m = 0;
             m < sizeof(k_feedback_modifiers) / sizeof(k_feedback_modifiers[0]);
             m++) {
            entries[entry++] = (struct np_dmabuf_format_table_entry) {
                .format = k_formats[f],
                .modifier = k_feedback_modifiers[m],
            };
        }
    }

    int fd = memfd_create("nativepipe-dmabuf-feedback",
                          MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    if (!write_all(fd, entries, sizeof(entries)) ||
        fcntl(fd, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void send_feedback(struct wl_resource *feedback, struct np_dmabuf *dmabuf)
{
    int table_fd = create_format_table();
    if (table_fd < 0) {
        wl_resource_post_no_memory(feedback);
        return;
    }

    struct wl_array device;
    struct wl_array explicit_indices;
    struct wl_array implicit_indices;
    wl_array_init(&device);
    wl_array_init(&explicit_indices);
    wl_array_init(&implicit_indices);

    dev_t *device_value = wl_array_add(&device, sizeof(*device_value));
    const size_t format_count = sizeof(k_formats) / sizeof(k_formats[0]);
    const size_t modifier_count =
        sizeof(k_feedback_modifiers) / sizeof(k_feedback_modifiers[0]);
    uint16_t *explicit_values = wl_array_add(
        &explicit_indices, sizeof(uint16_t) * format_count * 2);
    uint16_t *implicit_values = wl_array_add(
        &implicit_indices, sizeof(uint16_t) * format_count);
    if (!device_value || !explicit_values || !implicit_values) {
        wl_array_release(&implicit_indices);
        wl_array_release(&explicit_indices);
        wl_array_release(&device);
        close(table_fd);
        wl_resource_post_no_memory(feedback);
        return;
    }

    *device_value = dmabuf->device;
    for (size_t f = 0; f < format_count; f++) {
        explicit_values[f * 2] = (uint16_t)(f * modifier_count);
        explicit_values[f * 2 + 1] = (uint16_t)(f * modifier_count + 1);
        implicit_values[f] = (uint16_t)(f * modifier_count + 2);
    }

    zwp_linux_dmabuf_feedback_v1_send_format_table(
        feedback, table_fd, (uint32_t)(
            sizeof(struct np_dmabuf_format_table_entry) *
            format_count * modifier_count));
    zwp_linux_dmabuf_feedback_v1_send_main_device(feedback, &device);

    /* Every buffer selected from this feedback is a compositor presentation
     * candidate. SCANOUT is NativePipe's allocation marker for a native Metal
     * backing; it does not change the later host composition step. */
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback, &device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(
        feedback, ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT);
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(
        feedback, &explicit_indices);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback);

    /* Classic VirGL has no resource_create_with_modifiers callback, so its
     * second-choice implicit tranche carries the same native-backing marker. */
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback, &device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(
        feedback, ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT);
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(
        feedback, &implicit_indices);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback);
    zwp_linux_dmabuf_feedback_v1_send_done(feedback);

    wl_array_release(&implicit_indices);
    wl_array_release(&explicit_indices);
    wl_array_release(&device);
    close(table_fd);
}

static void create_feedback(struct wl_client *client, struct wl_resource *resource,
                            uint32_t id)
{
    struct wl_resource *feedback = wl_resource_create(
        client, &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
    if (!feedback) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(feedback, &feedback_implementation, NULL, NULL);
    send_feedback(feedback, wl_resource_get_user_data(resource));
}

static void get_default_feedback(struct wl_client *client,
                                 struct wl_resource *resource, uint32_t id)
{
    create_feedback(client, resource, id);
}

static void get_surface_feedback(struct wl_client *client,
                                 struct wl_resource *resource, uint32_t id,
                                 struct wl_resource *surface)
{
    (void)surface;
    create_feedback(client, resource, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_implementation = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_create_params,
    .get_default_feedback = get_default_feedback,
    .get_surface_feedback = get_surface_feedback,
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
    if (version > 4)
        version = 4;
    struct wl_resource *resource = wl_resource_create(
        client, &zwp_linux_dmabuf_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &dmabuf_implementation, dmabuf, NULL);
    /* format/modifier events are forbidden for v4 bindings; those clients get
     * the same pairs from the immutable feedback table instead. */
    if (version < 4)
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
    dmabuf->device = st.st_rdev;

    wl_global_create(display, &zwp_linux_dmabuf_v1_interface, 4, dmabuf, dmabuf_bind);
    fprintf(stderr, "[wayland] linux-dmabuf v4 exposes existing Venus textures\n");
}

struct np_gpu_buffer *np_gpu_buffer_get(struct wl_resource *buffer)
{
    if (!buffer ||
        !wl_resource_instance_of(buffer, &wl_buffer_interface, &gpu_buffer_implementation))
        return NULL;

    struct np_gpu_buffer_object *gpu = wl_resource_get_user_data(buffer);
    if (!gpu)
        return NULL;

    return &gpu->info;
}
