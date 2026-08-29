#define _GNU_SOURCE

/* Standard single-plane linear linux-dmabuf import for a real Linux host.
 * RemotePipe copies the producer image into its encoder, so it needs neither a
 * virtio resource id nor a second GPU context. */

#include "dmabuf.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "syncobj.h"

#include <errno.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_MOD_LINEAR 0ull
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffull

struct np_params {
	int fd;
	uint32_t offset;
	uint32_t stride;
	uint64_t modifier;
	bool has_plane;
	bool used;
};

struct np_gpu_buffer_object {
	struct np_gpu_buffer info;
	int fd;
	uint32_t offset;
	void *mapping;
	size_t mapping_size;
	struct wl_resource *resource;
	uint32_t references;
	uint32_t current_references;
	uint32_t cpu_reads;
};

static const struct wl_buffer_interface gpu_buffer_implementation;

static struct np_gpu_buffer_object *gpu_object(struct np_gpu_buffer *buffer)
{
	return buffer ? (struct np_gpu_buffer_object *)buffer : NULL;
}

static void gpu_ref(struct np_gpu_buffer_object *gpu)
{
	if (gpu) gpu->references++;
}

static void gpu_unref(struct np_gpu_buffer_object *gpu)
{
	if (!gpu || !gpu->references || --gpu->references) return;
	if (gpu->mapping && gpu->mapping != MAP_FAILED)
		munmap(gpu->mapping, gpu->mapping_size);
	if (gpu->fd >= 0) close(gpu->fd);
	free(gpu);
}

void np_gpu_buffer_retain(struct np_gpu_buffer *buffer)
{
	gpu_ref(gpu_object(buffer));
}

void np_gpu_buffer_drop(struct np_gpu_buffer *buffer)
{
	gpu_unref(gpu_object(buffer));
}

void np_gpu_buffer_acquire_current(struct np_gpu_buffer *buffer)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (!gpu) return;
	gpu->current_references++;
	gpu_ref(gpu);
}

void np_gpu_buffer_release_current(struct np_gpu_buffer *buffer)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (!gpu || !gpu->current_references) return;
	gpu->current_references--;
	gpu_unref(gpu);
}

enum np_gpu_read_result np_gpu_buffer_render_status(
	struct np_gpu_buffer *buffer, int *wait_fd)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (wait_fd) *wait_fd = -1;
	if (!gpu) return NP_GPU_READ_FAILED;

	struct dma_buf_export_sync_file export = {
		.flags = DMA_BUF_SYNC_READ,
		.fd = -1,
	};
	if (ioctl(gpu->fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export) < 0) {
		/* Older dma-buf exporters synchronize DMA_BUF_IOCTL_SYNC instead. */
		return errno == ENOTTY || errno == EINVAL
			? NP_GPU_READ_READY : NP_GPU_READ_FAILED;
	}
	if (export.fd < 0) return NP_GPU_READ_READY;
	struct pollfd descriptor = {.fd = export.fd, .events = POLLIN};
	int ready;
	do ready = poll(&descriptor, 1, 0); while (ready < 0 && errno == EINTR);
	if (ready > 0) {
		close(export.fd);
		return NP_GPU_READ_READY;
	}
	if (ready == 0) {
		if (wait_fd) *wait_fd = export.fd;
		else close(export.fd);
		return NP_GPU_READ_WAIT;
	}
	close(export.fd);
	return NP_GPU_READ_FAILED;
}

bool np_gpu_buffer_acquire_host_read(struct np_gpu_buffer *buffer)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (!gpu) return false;
	gpu->cpu_reads++;
	gpu_ref(gpu);
	return true;
}

void np_gpu_buffer_end_host_read(struct np_gpu_buffer *buffer)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (!gpu || !gpu->cpu_reads) return;
	gpu->cpu_reads--;
	gpu_unref(gpu);
}

bool np_gpu_buffer_begin_cpu_read(
	struct np_gpu_buffer *buffer, const unsigned char **pixels)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (pixels) *pixels = NULL;
	if (!gpu || !pixels || !gpu->mapping || gpu->mapping == MAP_FAILED) return false;
	struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
	if (ioctl(gpu->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0 && errno != ENOTTY)
		return false;
	gpu->cpu_reads++;
	gpu_ref(gpu);
	*pixels = (const unsigned char *)gpu->mapping + gpu->offset;
	return true;
}

void np_gpu_buffer_end_cpu_read(struct np_gpu_buffer *buffer)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	if (!gpu || !gpu->cpu_reads) return;
	struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
	if (ioctl(gpu->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0 && errno != ENOTTY)
		fprintf(stderr, "[wayland] DMA_BUF_SYNC END: %s\n", strerror(errno));
	gpu->cpu_reads--;
	gpu_unref(gpu);
}

bool np_gpu_buffer_is_busy(struct np_gpu_buffer *buffer)
{
	struct np_gpu_buffer_object *gpu = gpu_object(buffer);
	return gpu && gpu->cpu_reads != 0;
}

void np_gpu_buffer_queue_release(
	struct np_gpu_buffer *buffer, struct np_sync_point *point)
{
	(void)buffer;
	np_sync_point_signal(point);
}

static void gpu_resource_destroy(struct wl_resource *resource)
{
	struct np_gpu_buffer_object *gpu = wl_resource_get_user_data(resource);
	if (!gpu) return;
	gpu->resource = NULL;
	gpu_unref(gpu);
}

static void gpu_destroy_request(
	struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface gpu_buffer_implementation = {
	.destroy = gpu_destroy_request,
};

static bool supported_format(uint32_t format)
{
	return format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888;
}

static bool required_size(
	const struct np_params *params, int32_t width, int32_t height,
	uint64_t *required)
{
	if (width <= 0 || height <= 0) return false;
	uint64_t row = (uint64_t)(uint32_t)width * 4u;
	uint64_t last = (uint64_t)(uint32_t)(height - 1) * params->stride;
	uint64_t end = (uint64_t)params->offset + last + row;
	if (params->stride < row || end < last || end > SIZE_MAX) return false;
	*required = end;
	return true;
}

static struct wl_resource *make_gpu_buffer(
	struct wl_client *client, uint32_t id, struct np_params *params,
	int32_t width, int32_t height, uint32_t format)
{
	uint64_t required;
	if (!required_size(params, width, height, &required)) return NULL;
	struct stat status;
	off_t size = fstat(params->fd, &status) == 0 ? status.st_size : -1;
	if (size == 0) size = lseek(params->fd, 0, SEEK_END);
	if (size <= 0 || required > (uint64_t)size) return NULL;

	void *mapping = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, params->fd, 0);
	if (mapping == MAP_FAILED) return NULL;
	struct np_gpu_buffer_object *gpu = calloc(1, sizeof(*gpu));
	if (!gpu) {
		munmap(mapping, (size_t)size);
		return NULL;
	}
	gpu->fd = params->fd;
	gpu->offset = params->offset;
	gpu->mapping = mapping;
	gpu->mapping_size = (size_t)size;
	gpu->references = 1;
	gpu->info.resource_id = 0;
	gpu->info.width = width;
	gpu->info.height = height;
	gpu->info.stride = (int32_t)params->stride;
	/* The presentation/encoder API uses wl_shm's normalized BGRA enum;
	 * retain the DRM fourcc separately for EGL import. */
	gpu->info.format = format == DRM_FORMAT_ARGB8888
		? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;
	params->fd = -1;
	params->has_plane = false;

	struct wl_resource *resource = wl_resource_create(
		client, &wl_buffer_interface, 1, id);
	if (!resource) {
		gpu_unref(gpu);
		return NULL;
	}
	gpu->resource = resource;
	wl_resource_set_implementation(
		resource, &gpu_buffer_implementation, gpu, gpu_resource_destroy);
	return resource;
}

static void params_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void params_add(
	struct wl_client *client, struct wl_resource *resource, int32_t fd,
	uint32_t plane, uint32_t offset, uint32_t stride,
	uint32_t modifier_hi, uint32_t modifier_lo)
{
	(void)client;
	struct np_params *params = wl_resource_get_user_data(resource);
	if (params->used) {
		close(fd);
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "params already used");
		return;
	}
	if (plane != 0) {
		close(fd);
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "only plane 0 is supported");
		return;
	}
	if (params->has_plane) {
		close(fd);
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET, "plane 0 already set");
		return;
	}
	params->fd = fd;
	params->offset = offset;
	params->stride = stride;
	params->modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
	params->has_plane = true;
}

static bool validate_create(
	struct wl_resource *resource, struct np_params *params,
	int32_t width, int32_t height, uint32_t format)
{
	if (params->used) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "params already used");
		return false;
	}
	params->used = true;
	if (!params->has_plane || params->fd < 0) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "plane 0 is missing");
		return false;
	}
	if (!supported_format(format) ||
	    (params->modifier != DRM_FORMAT_MOD_LINEAR &&
	     params->modifier != DRM_FORMAT_MOD_INVALID)) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			"only linear ARGB8888/XRGB8888 is supported");
		return false;
	}
	uint64_t required;
	if (!required_size(params, width, height, &required)) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			"invalid dimensions, stride or offset");
		return false;
	}
	struct stat status;
	off_t size = fstat(params->fd, &status) == 0 ? status.st_size : -1;
	if (size == 0) size = lseek(params->fd, 0, SEEK_END);
	if (size <= 0 || required > (uint64_t)size) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			"plane extends outside dma-buf");
		return false;
	}
	return true;
}

static void params_create(
	struct wl_client *client, struct wl_resource *resource,
	int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
	struct np_params *params = wl_resource_get_user_data(resource);
	if (!validate_create(resource, params, width, height, format)) return;
	if (flags != 0) {
		zwp_linux_buffer_params_v1_send_failed(resource);
		return;
	}
	struct wl_resource *buffer = make_gpu_buffer(
		client, 0, params, width, height, format);
	if (!buffer) zwp_linux_buffer_params_v1_send_failed(resource);
	else zwp_linux_buffer_params_v1_send_created(resource, buffer);
}

static void params_create_immed(
	struct wl_client *client, struct wl_resource *resource, uint32_t id,
	int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
	struct np_params *params = wl_resource_get_user_data(resource);
	if (!validate_create(resource, params, width, height, format)) return;
	if (flags != 0 || !make_gpu_buffer(client, id, params, width, height, format))
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
			"could not map dma-buf");
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
	if (!params) return;
	if (params->fd >= 0) close(params->fd);
	free(params);
}

static void dmabuf_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void dmabuf_create_params(
	struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct np_params *params = calloc(1, sizeof(*params));
	if (!params) {
		wl_client_post_no_memory(client);
		return;
	}
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
	wl_resource_set_implementation(
		created, &params_implementation, params, params_resource_destroy);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_implementation = {
	.destroy = dmabuf_destroy,
	.create_params = dmabuf_create_params,
};

static void dmabuf_bind(
	struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	if (version > 3) version = 3;
	struct wl_resource *resource = wl_resource_create(
		client, &zwp_linux_dmabuf_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &dmabuf_implementation, NULL, NULL);
	zwp_linux_dmabuf_v1_send_format(resource, DRM_FORMAT_ARGB8888);
	zwp_linux_dmabuf_v1_send_format(resource, DRM_FORMAT_XRGB8888);
	if (version >= 3) {
		zwp_linux_dmabuf_v1_send_modifier(resource, DRM_FORMAT_ARGB8888, 0, 0);
		zwp_linux_dmabuf_v1_send_modifier(resource, DRM_FORMAT_XRGB8888, 0, 0);
	}
}

void np_dmabuf_advertise(struct wl_display *display, int drm_fd)
{
	(void)drm_fd;
	if (!display) return;
	wl_global_create(
		display, &zwp_linux_dmabuf_v1_interface, 3, NULL, dmabuf_bind);
	fprintf(stderr, "[wayland] linux-dmabuf v3 exposes linear ARGB/XRGB buffers\n");
}

struct np_gpu_buffer *np_gpu_buffer_get(struct wl_resource *buffer)
{
	if (!buffer || !wl_resource_instance_of(
			buffer, &wl_buffer_interface, &gpu_buffer_implementation))
		return NULL;
	struct np_gpu_buffer_object *gpu = wl_resource_get_user_data(buffer);
	return gpu ? &gpu->info : NULL;
}
