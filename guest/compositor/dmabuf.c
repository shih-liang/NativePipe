#define _GNU_SOURCE
// linux-dmabuf → virtio-gpu resource id.
//
// Guest Mesa Venus renders on the host (SUBMIT_3D → MoltenVK). The
// resulting image is a HOST3D blob. Mesa exports that blob as a dmabuf
// and attaches it to a wl_surface. We do not copy it. We name it.

#include "dmabuf.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"

#include <drm/drm.h>
#include <drm/virtgpu_drm.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_ABGR8888 0x34324241u
#define DRM_FORMAT_XBGR8888 0x34324258u

struct np_dmabuf {
	int drm_fd;
};

struct np_params {
	int drm_fd;
	int fd;
	uint32_t stride;
	bool has_plane;
};

struct np_gpu_buffer_object {
	struct np_gpu_buffer info;
	uint32_t bo_handle;
	int drm_fd;
};

/// Open the same DRM render node as a new file description, deliberately
/// without CONTEXT_INIT.  PRIME_FD_TO_HANDLE only needs GEM lookup.  Reusing
/// the compositor's Venus context fd makes the kernel emit CTX_ATTACH_RESOURCE
/// for every client's swapchain image; the compositor never submits GPU work,
/// and virglrenderer's proxy cannot import an MTLHeap into that second context.
static int open_prime_lookup_fd(int context_fd) {
	struct stat target;
	if (fstat(context_fd, &target) < 0) return -1;
	for (int minor = 128; minor <= 191; minor++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		struct stat candidate;
		if (fstat(fd, &candidate) == 0 && candidate.st_rdev == target.st_rdev) {
			fprintf(stderr, "[wayland] dmabuf lookup fd %s (no GPU context)\n", path);
			return fd;
		}
		close(fd);
	}
	errno = ENODEV;
	return -1;
}

static uint32_t resource_from_prime(int drm_fd, int prime_fd, uint32_t *bo_out) {
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
	if (ioctl(drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info) < 0) {
		fprintf(stderr, "[wayland] RESOURCE_INFO: %s\n", strerror(errno));
		struct drm_gem_close closer;
		memset(&closer, 0, sizeof(closer));
		closer.handle = prime.handle;
		ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &closer);
		return 0;
	}
	*bo_out = prime.handle;
	return info.res_handle;
}

static void gpu_buffer_destroy(struct wl_resource *resource) {
	struct np_gpu_buffer_object *gpu = wl_resource_get_user_data(resource);
	if (!gpu) return;
	if (gpu->bo_handle) {
		struct drm_gem_close closer;
		memset(&closer, 0, sizeof(closer));
		closer.handle = gpu->bo_handle;
		ioctl(gpu->drm_fd, DRM_IOCTL_GEM_CLOSE, &closer);
	}
	free(gpu);
}

static void gpu_buffer_destroy_request(struct wl_client *client, struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface gpu_buffer_implementation = {
	.destroy = gpu_buffer_destroy_request,
};

static struct wl_resource *make_gpu_buffer(struct wl_client *client, uint32_t id,
                                           struct np_params *params,
                                           int32_t width, int32_t height,
                                           uint32_t format) {
	if (!params->has_plane || params->fd < 0 || width <= 0 || height <= 0) {
		return NULL;
	}

	uint32_t bo = 0;
	uint32_t res = resource_from_prime(params->drm_fd, params->fd, &bo);
	close(params->fd);
	params->fd = -1;
	params->has_plane = false;
	if (!res) return NULL;

	struct np_gpu_buffer_object *gpu = calloc(1, sizeof(*gpu));
	if (!gpu) {
		struct drm_gem_close closer;
		memset(&closer, 0, sizeof(closer));
		closer.handle = bo;
		ioctl(params->drm_fd, DRM_IOCTL_GEM_CLOSE, &closer);
		return NULL;
	}
	gpu->info.resource_id = res;
	gpu->info.width = width;
	gpu->info.height = height;
	gpu->info.stride = (int32_t)params->stride;
	gpu->info.format = format;
	gpu->bo_handle = bo;
	gpu->drm_fd = params->drm_fd;

	struct wl_resource *buffer = wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (!buffer) {
		gpu_buffer_destroy(NULL);
		free(gpu);
		return NULL;
	}
	wl_resource_set_implementation(buffer, &gpu_buffer_implementation, gpu, gpu_buffer_destroy);
	fprintf(stderr, "[wayland] gpu buffer res=%u %dx%d stride=%u\n",
	        res, width, height, params->stride);
	return buffer;
}

static void params_destroy(struct wl_client *client, struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static void params_add(struct wl_client *client, struct wl_resource *resource,
                       int32_t fd, uint32_t plane_idx, uint32_t offset,
                       uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo) {
	(void)client;
	(void)offset;
	(void)modifier_hi;
	(void)modifier_lo;
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
	params->fd = fd;
	params->stride = stride;
	params->has_plane = true;
}

static void params_create(struct wl_client *client, struct wl_resource *resource,
                          int32_t width, int32_t height, uint32_t format, uint32_t flags) {
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
                                uint32_t format, uint32_t flags) {
	(void)flags;
	struct np_params *params = wl_resource_get_user_data(resource);
	struct wl_resource *buffer = make_gpu_buffer(
		client, buffer_id, params, width, height, format);
	if (!buffer) {
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
		                       "dmabuf is not a virtio-gpu blob");
	}
}

static const struct zwp_linux_buffer_params_v1_interface params_implementation = {
	.destroy = params_destroy,
	.add = params_add,
	.create = params_create,
	.create_immed = params_create_immed,
};

static void params_resource_destroy(struct wl_resource *resource) {
	struct np_params *params = wl_resource_get_user_data(resource);
	if (!params) return;
	if (params->fd >= 0) close(params->fd);
	free(params);
}

static void dmabuf_destroy(struct wl_client *client, struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static void dmabuf_create_params(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t id) {
	struct np_dmabuf *dmabuf = wl_resource_get_user_data(resource);
	struct np_params *params = calloc(1, sizeof(*params));
	if (!params) {
		wl_client_post_no_memory(client);
		return;
	}
	params->drm_fd = dmabuf->drm_fd;
	params->fd = -1;
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
	DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
};
static const uint64_t k_modifiers[] = {
	0ull, /* DRM_FORMAT_MOD_LINEAR */
	0x00ffffffffffffffull, /* DRM_FORMAT_MOD_INVALID */
};

struct feedback_table_entry {
	uint32_t format;
	uint32_t pad;
	uint64_t modifier;
};

static int make_format_table(uint32_t *size_out) {
	struct feedback_table_entry entries[8];
	unsigned n = 0;
	for (unsigned f = 0; f < 4; f++) {
		for (unsigned m = 0; m < 2; m++) {
			entries[n].format = k_formats[f];
			entries[n].pad = 0;
			entries[n].modifier = k_modifiers[m];
			n++;
		}
	}
	*size_out = (uint32_t)(n * sizeof(entries[0]));
	int fd = memfd_create("np-dmabuf-fmt", MFD_CLOEXEC);
	if (fd < 0) return -1;
	if (ftruncate(fd, (off_t)*size_out) < 0) {
		close(fd);
		return -1;
	}
	void *map = mmap(NULL, *size_out, PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return -1;
	}
	memcpy(map, entries, *size_out);
	munmap(map, *size_out);
	return fd;
}

static void send_feedback(struct wl_resource *feedback, int drm_fd) {
	uint32_t table_size = 0;
	int table_fd = make_format_table(&table_size);
	if (table_fd < 0) {
		fprintf(stderr, "[wayland] dmabuf feedback table: %s\n", strerror(errno));
		return;
	}
	fprintf(stderr, "[wayland] dmabuf feedback table=%u bytes drm_fd=%d\n",
	        table_size, drm_fd);
	zwp_linux_dmabuf_feedback_v1_send_format_table(feedback, table_fd, table_size);
	close(table_fd);

	struct stat st;
	memset(&st, 0, sizeof(st));
	if (fstat(drm_fd, &st) < 0)
		fprintf(stderr, "[wayland] dmabuf fstat: %s\n", strerror(errno));

	struct wl_array device;
	wl_array_init(&device);
	dev_t *slot = wl_array_add(&device, sizeof(dev_t));
	if (slot) *slot = st.st_rdev;
	zwp_linux_dmabuf_feedback_v1_send_main_device(feedback, &device);
	zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback, &device);

	struct wl_array indices;
	wl_array_init(&indices);
	for (uint16_t i = 0; i < 8; i++) {
		uint16_t *idx = wl_array_add(&indices, sizeof(uint16_t));
		if (idx) *idx = i;
	}
	zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback, &indices);
	zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback, 0);
	zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback);
	zwp_linux_dmabuf_feedback_v1_send_done(feedback);
	wl_array_release(&indices);
	wl_array_release(&device);
}

static void feedback_destroy(struct wl_client *client, struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_implementation = {
	.destroy = feedback_destroy,
};

static void dmabuf_get_default_feedback(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t id) {
	struct np_dmabuf *dmabuf = wl_resource_get_user_data(resource);
	struct wl_resource *feedback = wl_resource_create(
		client, &zwp_linux_dmabuf_feedback_v1_interface,
		wl_resource_get_version(resource), id);
	if (!feedback) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(feedback, &feedback_implementation, NULL, NULL);
	send_feedback(feedback, dmabuf->drm_fd);
}

static void dmabuf_get_surface_feedback(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t id, struct wl_resource *surface) {
	(void)surface;
	dmabuf_get_default_feedback(client, resource, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_implementation = {
	.destroy = dmabuf_destroy,
	.create_params = dmabuf_create_params,
	.get_default_feedback = dmabuf_get_default_feedback,
	.get_surface_feedback = dmabuf_get_surface_feedback,
};

static void send_formats(struct wl_resource *resource) {
	// v3 clients still need format+modifier events. v4 uses feedback
	// instead; sending both is harmless and Mesa ignores these when
	// feedback is present.
	for (size_t i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); i++) {
		zwp_linux_dmabuf_v1_send_format(resource, k_formats[i]);
		if (wl_resource_get_version(resource) >= 3) {
			for (size_t m = 0; m < 2; m++) {
				zwp_linux_dmabuf_v1_send_modifier(
					resource, k_formats[i],
					(uint32_t)(k_modifiers[m] >> 32),
					(uint32_t)k_modifiers[m]);
			}
		}
	}
}

static void dmabuf_bind(struct wl_client *client, void *data,
                        uint32_t version, uint32_t id) {
	struct np_dmabuf *dmabuf = data;
	struct wl_resource *resource = wl_resource_create(
		client, &zwp_linux_dmabuf_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &dmabuf_implementation, dmabuf, NULL);
	send_formats(resource);
}

void np_dmabuf_advertise(struct wl_display *display, int drm_fd) {
	struct np_dmabuf *dmabuf = calloc(1, sizeof(*dmabuf));
	if (!dmabuf) return;
	dmabuf->drm_fd = open_prime_lookup_fd(drm_fd);
	if (dmabuf->drm_fd < 0) {
		fprintf(stderr, "[wayland] could not open context-free dmabuf lookup fd: %s\n",
		        strerror(errno));
		free(dmabuf);
		return;
	}
	// Version 3 only. v4 feedback made Mesa Venus roundtrip the Wayland
	// display re-entrantly from CreateSwapchain; Alpine vkcube then
	// SIGSEGV'd in libwayland free(). Modifier events are enough for WSI.
	wl_global_create(display, &zwp_linux_dmabuf_v1_interface, 3, dmabuf, dmabuf_bind);
}

struct np_gpu_buffer *np_gpu_buffer_get(struct wl_resource *buffer) {
	if (!buffer) return NULL;
	if (wl_resource_instance_of(buffer, &wl_buffer_interface, &gpu_buffer_implementation)) {
		struct np_gpu_buffer_object *gpu = wl_resource_get_user_data(buffer);
		return gpu ? &gpu->info : NULL;
	}
	return NULL;
}
