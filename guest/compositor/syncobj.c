#define _GNU_SOURCE

#include "syncobj.h"

#include "compositor_internal.h"
#include "dmabuf.h"
#include "linux-drm-syncobj-v1-server-protocol.h"

#include <drm.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct np_sync_timeline {
	int drm_fd;
	uint32_t handle;
	uint32_t references;
};

struct np_sync_point {
	struct np_sync_timeline *timeline;
	uint64_t value;
};

struct np_sync_surface {
	struct wl_resource *resource;
	struct np_surface *surface;
	struct np_sync_point *pending_acquire;
	struct np_sync_point *pending_release;
};

static void timeline_ref(struct np_sync_timeline *timeline)
{
	if (timeline) timeline->references++;
}

static void timeline_unref(struct np_sync_timeline *timeline)
{
	if (!timeline || !timeline->references || --timeline->references) return;
	struct drm_syncobj_destroy destroy = { .handle = timeline->handle };
	if (ioctl(timeline->drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
		fprintf(stderr, "[wayland] SYNCOBJ_DESTROY: %s\n", strerror(errno));
	free(timeline);
}

static struct np_sync_point *point_create(
	struct np_sync_timeline *timeline, uint64_t value)
{
	if (!timeline) return NULL;
	struct np_sync_point *point = calloc(1, sizeof(*point));
	if (!point) return NULL;
	point->timeline = timeline;
	point->value = value;
	timeline_ref(timeline);
	return point;
}

void np_sync_point_destroy(struct np_sync_point *point)
{
	if (!point) return;
	timeline_unref(point->timeline);
	free(point);
}

bool np_sync_point_ready(struct np_sync_point *point)
{
	if (!point) return true;
	uint32_t handle = point->timeline->handle;
	uint64_t value = point->value;
	struct drm_syncobj_timeline_wait wait = {
		.handles = (uintptr_t)&handle,
		.points = (uintptr_t)&value,
		.timeout_nsec = 0,
		.count_handles = 1,
		.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
		         DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
	};
	if (ioctl(point->timeline->drm_fd,
	          DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait) == 0)
		return true;
	if (errno == ETIME || errno == ETIMEDOUT || errno == EBUSY || errno == EAGAIN)
		return false;
	fprintf(stderr, "[wayland] SYNCOBJ_TIMELINE_WAIT: %s\n", strerror(errno));
	return false;
}

int np_sync_point_wait_fd(struct np_sync_point *point)
{
	if (!point) return -1;
	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fd < 0) return -1;
	struct drm_syncobj_eventfd event = {
		.handle = point->timeline->handle,
		.point = point->value,
		.fd = fd,
	};
	if (ioctl(point->timeline->drm_fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &event) < 0) {
		fprintf(stderr, "[wayland] SYNCOBJ_EVENTFD: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

void np_sync_point_signal(struct np_sync_point *point)
{
	if (!point) return;
	uint32_t handle = point->timeline->handle;
	uint64_t value = point->value;
	struct drm_syncobj_timeline_array signal = {
		.handles = (uintptr_t)&handle,
		.points = (uintptr_t)&value,
		.count_handles = 1,
	};
	if (ioctl(point->timeline->drm_fd,
	          DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &signal) < 0)
		fprintf(stderr, "[wayland] SYNCOBJ_TIMELINE_SIGNAL: %s\n", strerror(errno));
	np_sync_point_destroy(point);
}

static void timeline_resource_destroy(struct wl_resource *resource)
{
	timeline_unref(wl_resource_get_user_data(resource));
}

static void timeline_destroy_request(
	struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wp_linux_drm_syncobj_timeline_v1_interface
timeline_implementation = {
	.destroy = timeline_destroy_request,
};

static void sync_surface_resource_destroy(struct wl_resource *resource)
{
	struct np_sync_surface *sync = wl_resource_get_user_data(resource);
	if (!sync) return;
	if (sync->surface && sync->surface->syncobj == sync)
		sync->surface->syncobj = NULL;
	np_sync_point_destroy(sync->pending_acquire);
	np_sync_point_destroy(sync->pending_release);
	free(sync);
}

static void sync_surface_destroy_request(
	struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void set_point(struct wl_client *client, struct wl_resource *resource,
	                  struct wl_resource *timeline_resource,
	                  uint32_t high, uint32_t low, bool acquire)
{
	struct np_sync_surface *sync = wl_resource_get_user_data(resource);
	if (!sync || !sync->surface) {
		wl_resource_post_error(resource,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
			"the wl_surface no longer exists");
		return;
	}
	struct np_sync_timeline *timeline =
		wl_resource_get_user_data(timeline_resource);
	struct np_sync_point *point = point_create(
		timeline, ((uint64_t)high << 32) | low);
	if (!point) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_sync_point **slot = acquire
		? &sync->pending_acquire : &sync->pending_release;
	np_sync_point_destroy(*slot);
	*slot = point;
}

static void sync_surface_set_acquire(
	struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *timeline, uint32_t high, uint32_t low)
{
	set_point(client, resource, timeline, high, low, true);
}

static void sync_surface_set_release(
	struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *timeline, uint32_t high, uint32_t low)
{
	set_point(client, resource, timeline, high, low, false);
}

static const struct wp_linux_drm_syncobj_surface_v1_interface
sync_surface_implementation = {
	.destroy = sync_surface_destroy_request,
	.set_acquire_point = sync_surface_set_acquire,
	.set_release_point = sync_surface_set_release,
};

static void manager_destroy(
	struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void manager_get_surface(
	struct wl_client *client, struct wl_resource *resource, uint32_t id,
	struct wl_resource *surface_resource)
{
	(void)resource;
	struct np_surface *surface = wl_resource_get_user_data(surface_resource);
	if (!surface) return;
	if (surface->syncobj) {
		wl_resource_post_error(resource,
			WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"surface already has an explicit sync object");
		return;
	}
	struct np_sync_surface *sync = calloc(1, sizeof(*sync));
	if (!sync) {
		wl_client_post_no_memory(client);
		return;
	}
	sync->surface = surface;
	sync->resource = wl_resource_create(
		client, &wp_linux_drm_syncobj_surface_v1_interface, 1, id);
	if (!sync->resource) {
		free(sync);
		wl_client_post_no_memory(client);
		return;
	}
	surface->syncobj = sync;
	wl_resource_set_implementation(sync->resource, &sync_surface_implementation,
	                               sync, sync_surface_resource_destroy);
}

static void manager_import_timeline(
	struct wl_client *client, struct wl_resource *resource, uint32_t id,
	int32_t fd)
{
	struct np_server *server = wl_resource_get_user_data(resource);
	struct drm_syncobj_handle import = { .fd = fd };
	int result = ioctl(server->drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import);
	int import_errno = errno;
	close(fd);
	if (result < 0 || !import.handle) {
		fprintf(stderr,
		        "[wayland] SYNCOBJ_FD_TO_HANDLE fd=%d result=%d handle=%u: %s\n",
		        fd, result, import.handle,
		        result < 0 ? strerror(import_errno) : "zero handle");
		wl_resource_post_error(resource,
			WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE,
			"could not import DRM syncobj timeline");
		return;
	}
	struct np_sync_timeline *timeline = calloc(1, sizeof(*timeline));
	if (!timeline) {
		struct drm_syncobj_destroy destroy = { .handle = import.handle };
		ioctl(server->drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
		wl_client_post_no_memory(client);
		return;
	}
	timeline->drm_fd = server->drm_fd;
	timeline->handle = import.handle;
	timeline->references = 1;
	struct wl_resource *timeline_resource = wl_resource_create(
		client, &wp_linux_drm_syncobj_timeline_v1_interface, 1, id);
	if (!timeline_resource) {
		timeline_unref(timeline);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(timeline_resource, &timeline_implementation,
	                               timeline, timeline_resource_destroy);
}

static const struct wp_linux_drm_syncobj_manager_v1_interface
manager_implementation = {
	.destroy = manager_destroy,
	.get_surface = manager_get_surface,
	.import_timeline = manager_import_timeline,
};

static void manager_bind(struct wl_client *client, void *data,
	                     uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(
		client, &wp_linux_drm_syncobj_manager_v1_interface,
		version > 1 ? 1 : version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_implementation, data, NULL);
}

void np_syncobj_advertise(struct wl_display *display, struct np_server *server)
{
	wl_global_create(display, &wp_linux_drm_syncobj_manager_v1_interface, 1,
	                 server, manager_bind);
}

bool np_syncobj_take_commit(
	struct np_surface *surface, bool buffer_set, struct wl_resource *buffer,
	struct np_sync_point **acquire, struct np_sync_point **release)
{
	*acquire = NULL;
	*release = NULL;
	struct np_sync_surface *sync = surface ? surface->syncobj : NULL;
	if (!sync) return true;
	bool has_buffer = buffer_set && buffer != NULL;
	if (!has_buffer) {
		if (sync->pending_acquire || sync->pending_release) {
			wl_resource_post_error(sync->resource,
				WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER,
				"sync points require a buffer in the same commit");
			return false;
		}
		return true;
	}
	if (!np_gpu_buffer_get(buffer)) {
		wl_resource_post_error(sync->resource,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER,
			"explicit sync is supported only for linux-dmabuf buffers");
		return false;
	}
	if (!sync->pending_acquire) {
		wl_resource_post_error(sync->resource,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT,
			"buffer commit has no acquire point");
		return false;
	}
	if (!sync->pending_release) {
		wl_resource_post_error(sync->resource,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT,
			"buffer commit has no release point");
		return false;
	}
	if (sync->pending_acquire->timeline == sync->pending_release->timeline &&
	    sync->pending_acquire->value >= sync->pending_release->value) {
		wl_resource_post_error(sync->resource,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS,
			"acquire point must precede release point on one timeline");
		return false;
	}
	*acquire = sync->pending_acquire;
	*release = sync->pending_release;
	sync->pending_acquire = NULL;
	sync->pending_release = NULL;
	return true;
}

bool np_syncobj_has_pending(struct np_surface *surface)
{
	struct np_sync_surface *sync = surface ? surface->syncobj : NULL;
	return sync && (sync->pending_acquire || sync->pending_release);
}

void np_syncobj_surface_destroyed(struct np_surface *surface)
{
	if (!surface || !surface->syncobj) return;
	struct np_sync_surface *sync = surface->syncobj;
	sync->surface = NULL;
	surface->syncobj = NULL;
}
