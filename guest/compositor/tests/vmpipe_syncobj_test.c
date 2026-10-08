/* Probe failures must leave explicit synchronization unadvertised. The DRM
 * collaborator is fake; notification delivery and fd cleanup use real eventfds. */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <wayland-server-core.h>

static int fixture_ioctl(int fd, unsigned long request, ...);
static int fixture_eventfd(unsigned int value, int flags);
static struct wl_global *fixture_global_create(
	struct wl_display *display, const struct wl_interface *interface,
	int version, void *data, wl_global_bind_func_t bind);
#define ioctl fixture_ioctl
#define eventfd fixture_eventfd
#define wl_global_create fixture_global_create
#include "../backends/vmpipe/syncobj.c"
#undef wl_global_create
#undef eventfd
#undef ioctl
#include <fcntl.h>

enum failure {
	NONE, CAP_QUERY, NO_SYNCOBJ, NO_TIMELINE, CREATE, CREATE_EVENTFD,
	SIGNAL, WAIT, REGISTER_EVENTFD, NO_NOTIFICATION, DESTROY,
};
static enum failure failure;
static unsigned globals, creates, destroys, ioctls;
static int notification_fd;
static bool signaled;

static int fixture_eventfd(unsigned int value, int flags)
{
	if (failure == CREATE_EVENTFD) { errno = EMFILE; return -1; }
	notification_fd = eventfd(value, flags);
	return notification_fd;
}

static int fixture_ioctl(int fd, unsigned long request, ...)
{
	assert(fd == 42);
	ioctls++;
	va_list args;
	va_start(args, request);
	void *argument = va_arg(args, void *);
	va_end(args);
	if (request == DRM_IOCTL_GET_CAP) {
		struct drm_get_cap *cap = argument;
		if (failure == CAP_QUERY) { errno = EINVAL; return -1; }
		assert(cap->capability == DRM_CAP_SYNCOBJ ||
		       cap->capability == DRM_CAP_SYNCOBJ_TIMELINE);
		cap->value = !((failure == NO_SYNCOBJ && cap->capability == DRM_CAP_SYNCOBJ) ||
		               (failure == NO_TIMELINE && cap->capability == DRM_CAP_SYNCOBJ_TIMELINE));
		return 0;
	}
	if (request == DRM_IOCTL_SYNCOBJ_CREATE) {
		if (failure == CREATE) { errno = ENOTTY; return -1; }
		struct drm_syncobj_create *create = argument;
		assert(create->flags == 0);
		create->handle = 17;
		creates++;
		return 0;
	}
	if (request == DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL) {
		if (failure == SIGNAL) { errno = ENOTTY; return -1; }
		struct drm_syncobj_timeline_array *signal = argument;
		assert(signal->count_handles == 1 &&
		       *(uint32_t *)(uintptr_t)signal->handles == 17 &&
		       *(uint64_t *)(uintptr_t)signal->points == 1);
		signaled = true;
		return 0;
	}
	if (request == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT) {
		if (failure == WAIT) { errno = ENOTTY; return -1; }
		struct drm_syncobj_timeline_wait *wait = argument;
		assert(signaled && wait->timeout_nsec == 0 && wait->count_handles == 1);
		assert(wait->flags == (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
		                      DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT));
		return 0;
	}
	if (request == DRM_IOCTL_SYNCOBJ_EVENTFD) {
		if (failure == REGISTER_EVENTFD) { errno = ENOTTY; return -1; }
		struct np_drm_syncobj_eventfd *event = argument;
		assert(signaled && event->handle == 17 && event->point == 1);
		assert(event->flags == 0 && event->fd == notification_fd);
		if (failure != NO_NOTIFICATION) {
			uint64_t value = 1;
			assert(write(event->fd, &value, sizeof(value)) == (ssize_t)sizeof(value));
		}
		return 0;
	}
	if (request == DRM_IOCTL_SYNCOBJ_DESTROY) {
		struct drm_syncobj_destroy *destroy = argument;
		assert(destroy->handle == 17);
		destroys++;
		if (failure == DESTROY) { errno = EIO; return -1; }
		return 0;
	}
	assert(!"unexpected DRM operation");
	return -1;
}

static struct wl_global *fixture_global_create(
	struct wl_display *display, const struct wl_interface *interface,
	int version, void *data, wl_global_bind_func_t bind)
{
	assert(display && interface == &wp_linux_drm_syncobj_manager_v1_interface);
	assert(version == 1 && data && bind);
	globals++;
	return NULL;
}

/* No client protocol operations run in this capability test. */
struct np_surface *np_surface_by_id(struct np_server *server, uint32_t id)
{ (void)server; (void)id; return NULL; }
struct np_gpu_buffer *np_gpu_buffer_get(struct wl_resource *buffer)
{ (void)buffer; return NULL; }

static void check_probe(struct np_server *server, enum failure next)
{
	failure = next;
	globals = creates = destroys = ioctls = 0;
	notification_fd = -1;
	signaled = false;
	np_syncobj_advertise(server->display, server);
	assert(globals == (next == NONE ? 1u : 0u));
	assert(creates == destroys); /* Includes every failure after creation. */
	if (notification_fd >= 0) {
		errno = 0;
		assert(fcntl(notification_fd, F_GETFD) == -1 && errno == EBADF);
	}
}

int main(void)
{
	struct np_vmpipe_backend backend = { .drm_fd = 42 };
	struct np_server server = {
		.display = wl_display_create(), .backend_state = &backend,
	};
	assert(server.display);
	for (enum failure next = NONE; next <= DESTROY; next++)
		check_probe(&server, next);
	backend.drm_fd = -1;
	check_probe(&server, NO_SYNCOBJ);
	assert(ioctls == 0);
	server.backend_state = NULL;
	check_probe(&server, NO_SYNCOBJ);
	assert(ioctls == 0);
	wl_display_destroy(server.display);
	puts("VM syncobj capability failures, successful advertisement and probe cleanup: PASS");
	return 0;
}
