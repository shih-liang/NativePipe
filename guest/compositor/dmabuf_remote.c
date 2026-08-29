#define _GNU_SOURCE

/* Standard single-plane linux-dmabuf import for a real Linux host. Linear
 * images take the direct CPU path; tiled images are imported and read back by
 * the matching EGL device before the remote encoder copies them. */

#include "dmabuf.h"
#include "dmabuf_egl.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "syncobj.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
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

struct np_dmabuf {
	dev_t device;
	char *render_node;
	struct np_egl_importer *importer;
	struct wl_listener display_destroy;
};

struct np_dmabuf_format_table_entry {
	uint32_t format;
	uint32_t padding;
	uint64_t modifier;
};

static const uint32_t k_formats[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,
};

struct np_params {
	struct np_dmabuf *dmabuf;
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
	struct np_egl_buffer *egl_buffer;
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
	np_egl_buffer_destroy(gpu->egl_buffer);
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
	if (!gpu || !pixels) return false;
	if (gpu->egl_buffer) {
		int32_t stride = 0;
		if (!np_egl_buffer_read_bgra(
				gpu->egl_buffer,
				gpu->info.format == WL_SHM_FORMAT_XRGB8888,
				pixels, &stride))
			return false;
		gpu->info.stride = stride;
		gpu->cpu_reads++;
		gpu_ref(gpu);
		return true;
	}
	if (!gpu->mapping || gpu->mapping == MAP_FAILED) return false;
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
	if (gpu->mapping && gpu->mapping != MAP_FAILED) {
		struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
		if (ioctl(gpu->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0 && errno != ENOTTY)
			fprintf(stderr, "[wayland] DMA_BUF_SYNC END: %s\n", strerror(errno));
	}
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

static bool supported_pair(
	const struct np_dmabuf *dmabuf, uint32_t format, uint64_t modifier)
{
	if (!supported_format(format)) return false;
	if (modifier == DRM_FORMAT_MOD_LINEAR) return true;
	return dmabuf && dmabuf->importer &&
	       np_egl_importer_supports(dmabuf->importer, format, modifier);
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
	struct np_gpu_buffer_object *gpu = calloc(1, sizeof(*gpu));
	if (!gpu) return NULL;
	gpu->fd = -1;
	gpu->offset = params->offset;
	gpu->references = 1;
	if (params->modifier == DRM_FORMAT_MOD_LINEAR) {
		uint64_t required;
		if (!required_size(params, width, height, &required)) goto failed;
		struct stat status;
		off_t size = fstat(params->fd, &status) == 0 ? status.st_size : -1;
		if (size == 0) size = lseek(params->fd, 0, SEEK_END);
		if (size <= 0 || required > (uint64_t)size) goto failed;
		gpu->mapping = mmap(
			NULL, (size_t)size, PROT_READ, MAP_SHARED, params->fd, 0);
		if (gpu->mapping == MAP_FAILED) goto failed;
		gpu->mapping_size = (size_t)size;
	} else {
		gpu->egl_buffer = np_egl_buffer_import(
			params->dmabuf->importer, params->fd, width, height, format,
			params->offset, params->stride, params->modifier);
		if (!gpu->egl_buffer) goto failed;
	}
	gpu->fd = params->fd;
	gpu->info.resource_id = 0;
	gpu->info.width = width;
	gpu->info.height = height;
	gpu->info.stride = params->modifier == DRM_FORMAT_MOD_LINEAR
		? (int32_t)params->stride : width * 4;
	/* The encoder API uses wl_shm's normalized BGRA enum, not DRM fourcc. */
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

failed:
	gpu_unref(gpu);
	return NULL;
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
	if (!supported_pair(params->dmabuf, format, params->modifier)) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			"format/modifier pair was not advertised");
		return false;
	}
	if (width <= 0 || height <= 0 || width > INT32_MAX / 4 ||
	    params->stride > INT32_MAX || params->offset > INT32_MAX) {
		wl_resource_post_error(resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			"invalid dimensions, stride or offset");
		return false;
	}
	if (params->modifier == DRM_FORMAT_MOD_LINEAR) {
		uint64_t required;
		if (!required_size(params, width, height, &required)) {
			wl_resource_post_error(resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
				"invalid linear dma-buf layout");
			return false;
		}
		struct stat status;
		off_t size = fstat(params->fd, &status) == 0 ? status.st_size : -1;
		if (size == 0) size = lseek(params->fd, 0, SEEK_END);
		if (size <= 0 || required > (uint64_t)size) {
			wl_resource_post_error(resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
				"plane extends outside linear dma-buf");
			return false;
		}
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
	params->dmabuf = wl_resource_get_user_data(resource);
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

static void feedback_destroy(
	struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_implementation = {
	.destroy = feedback_destroy,
};

static bool write_all(int fd, const void *data, size_t size)
{
	const unsigned char *bytes = data;
	while (size) {
		ssize_t written = write(fd, bytes, size);
		if (written < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		bytes += written;
		size -= (size_t)written;
	}
	return true;
}

static int create_format_table(struct np_dmabuf *dmabuf, uint32_t *size_out)
{
	*size_out = 0;
	size_t count = 0;
	const struct np_dmabuf_format_modifier *formats =
		np_egl_importer_formats(dmabuf->importer, &count);
	if (!formats || !count || count > UINT32_MAX /
	        sizeof(struct np_dmabuf_format_table_entry))
		return -1;
	struct np_dmabuf_format_table_entry *entries =
		calloc(count, sizeof(*entries));
	if (!entries) return -1;
	for (size_t index = 0; index < count; index++) {
		entries[index].format = formats[index].format;
		entries[index].modifier = formats[index].modifier;
	}
	int fd = memfd_create(
		"remotepipe-dmabuf-feedback", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) {
		free(entries);
		return -1;
	}
	size_t size = count * sizeof(*entries);
	if (!write_all(fd, entries, size) ||
	    fcntl(fd, F_ADD_SEALS,
	          F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
		free(entries);
		close(fd);
		return -1;
	}
	free(entries);
	*size_out = (uint32_t)size;
	return fd;
}

static void send_feedback(struct wl_resource *feedback, struct np_dmabuf *dmabuf)
{
	uint32_t table_size = 0;
	int table_fd = create_format_table(dmabuf, &table_size);
	if (table_fd < 0) {
		wl_resource_post_no_memory(feedback);
		return;
	}
	struct wl_array device;
	struct wl_array indices;
	wl_array_init(&device);
	wl_array_init(&indices);
	dev_t *device_value = wl_array_add(&device, sizeof(*device_value));
	size_t format_count = 0;
	np_egl_importer_formats(dmabuf->importer, &format_count);
	uint16_t *format_indices = format_count <= UINT16_MAX
		? wl_array_add(&indices, sizeof(uint16_t) * format_count) : NULL;
	if (!device_value || !format_indices) {
		wl_array_release(&indices);
		wl_array_release(&device);
		close(table_fd);
		wl_resource_post_no_memory(feedback);
		return;
	}
	*device_value = dmabuf->device;
	for (size_t index = 0; index < format_count; index++)
		format_indices[index] = (uint16_t)index;

	zwp_linux_dmabuf_feedback_v1_send_format_table(
		feedback, table_fd, table_size);
	zwp_linux_dmabuf_feedback_v1_send_main_device(feedback, &device);
	zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback, &device);
	zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback, 0);
	zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback, &indices);
	zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback);
	zwp_linux_dmabuf_feedback_v1_send_done(feedback);

	wl_array_release(&indices);
	wl_array_release(&device);
	close(table_fd);
}

static void create_feedback(
	struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *feedback = wl_resource_create(
		client, &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
	if (!feedback) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(
		feedback, &feedback_implementation, NULL, NULL);
	send_feedback(feedback, wl_resource_get_user_data(resource));
}

static void get_default_feedback(
	struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	create_feedback(client, resource, id);
}

static void get_surface_feedback(
	struct wl_client *client, struct wl_resource *resource, uint32_t id,
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

static void dmabuf_bind(
	struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	if (version > 4) version = 4;
	struct wl_resource *resource = wl_resource_create(
		client, &zwp_linux_dmabuf_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &dmabuf_implementation, data, NULL);
	/* v4 bindings receive the same pairs through immutable feedback. */
	if (version < 4) {
		for (size_t index = 0;
		     index < sizeof(k_formats) / sizeof(k_formats[0]); index++)
			zwp_linux_dmabuf_v1_send_format(resource, k_formats[index]);
		if (version >= 3 && data) {
			size_t count = 0;
			const struct np_dmabuf_format_modifier *formats =
				np_egl_importer_formats(((struct np_dmabuf *)data)->importer, &count);
			if (formats) {
				for (size_t index = 0; index < count; index++)
					zwp_linux_dmabuf_v1_send_modifier(
						resource, formats[index].format,
						(uint32_t)(formats[index].modifier >> 32),
						(uint32_t)formats[index].modifier);
			} else {
				for (size_t index = 0;
				     index < sizeof(k_formats) / sizeof(k_formats[0]); index++)
					zwp_linux_dmabuf_v1_send_modifier(
						resource, k_formats[index], 0, 0);
			}
		}
	}
}

static int open_render_node(char **path_out)
{
	const char *configured = getenv("REMOTEPIPE_RENDER_NODE");
	if (configured && configured[0]) {
		int fd = open(configured, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			*path_out = strdup(configured);
			return fd;
		}
		fprintf(stderr, "[wayland] cannot open REMOTEPIPE_RENDER_NODE=%s: %s\n",
		        configured, strerror(errno));
	}

	glob_t nodes = {0};
	if (glob("/dev/dri/renderD*", 0, NULL, &nodes) != 0) return -1;
	int fd = -1;
	for (size_t index = 0; index < nodes.gl_pathc; index++) {
		fd = open(nodes.gl_pathv[index], O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		*path_out = strdup(nodes.gl_pathv[index]);
		break;
	}
	globfree(&nodes);
	return fd;
}

static void dmabuf_display_destroyed(struct wl_listener *listener, void *data)
{
	(void)data;
	struct np_dmabuf *dmabuf = wl_container_of(
		listener, dmabuf, display_destroy);
	np_egl_importer_destroy(dmabuf->importer);
	free(dmabuf->render_node);
	free(dmabuf);
}

void np_dmabuf_advertise(struct wl_display *display, int drm_fd)
{
	if (!display) return;
	struct np_dmabuf *dmabuf = calloc(1, sizeof(*dmabuf));
	if (!dmabuf) return;
	bool close_fd = false;
	if (drm_fd < 0) {
		drm_fd = open_render_node(&dmabuf->render_node);
		close_fd = drm_fd >= 0;
	}
	struct stat status;
	bool has_device = drm_fd >= 0 && fstat(drm_fd, &status) == 0 &&
	                  S_ISCHR(status.st_mode);
	if (has_device) dmabuf->device = status.st_rdev;
	if (has_device) dmabuf->importer = np_egl_importer_create(drm_fd);
	if (close_fd) close(drm_fd);
	dmabuf->display_destroy.notify = dmabuf_display_destroyed;
	wl_display_add_destroy_listener(display, &dmabuf->display_destroy);
	wl_global_create(
		display, &zwp_linux_dmabuf_v1_interface,
		dmabuf->importer ? 4 : 3, dmabuf, dmabuf_bind);
	if (dmabuf->importer)
		fprintf(stderr,
		        "[wayland] linux-dmabuf v4 EGL feedback device=%s\n",
		        dmabuf->render_node ? dmabuf->render_node : "inherited drm fd");
	else
		fprintf(stderr,
		        "[wayland] no render node; linux-dmabuf v3 linear fallback\n");
}

struct np_gpu_buffer *np_gpu_buffer_get(struct wl_resource *buffer)
{
	if (!buffer || !wl_resource_instance_of(
			buffer, &wl_buffer_interface, &gpu_buffer_implementation))
		return NULL;
	struct np_gpu_buffer_object *gpu = wl_resource_get_user_data(buffer);
	return gpu ? &gpu->info : NULL;
}
