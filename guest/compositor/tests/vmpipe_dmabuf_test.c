/* Capture the real server's outgoing protocol events on real Wayland resources.
 * v3 must enumerate INVALID; v4 must keep its immutable feedback-only contract. */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <wayland-server-core.h>
static void capture_event(struct wl_resource *resource, uint32_t opcode, ...);
#define wl_resource_post_event capture_event
#include "../backends/vmpipe/dmabuf.c"
#undef wl_resource_post_event
#include <sys/socket.h>

static unsigned format_events, modifier_events, invalid_events[2], feedback_done;
static unsigned feedback_implicit;

uint64_t np_perf_now_ns(void) { return 0; }
void np_perf_record(enum np_perf_stage stage, uint64_t elapsed)
{ (void)stage; (void)elapsed; }
void np_sync_point_destroy(struct np_sync_point *point) { assert(!point); }
void np_sync_point_signal(struct np_sync_point *point) { assert(!point); }

static void capture_event(struct wl_resource *resource, uint32_t opcode, ...)
{
	va_list args;
	va_start(args, opcode);
	if (!strcmp(wl_resource_get_class(resource), "zwp_linux_dmabuf_v1")) {
		uint32_t format = va_arg(args, uint32_t);
		unsigned index = format == DRM_FORMAT_ARGB8888 ? 0 : 1;
		assert(format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888);
		if (opcode == ZWP_LINUX_DMABUF_V1_FORMAT) {
			format_events++;
		} else {
			assert(opcode == ZWP_LINUX_DMABUF_V1_MODIFIER);
			uint64_t modifier = (uint64_t)va_arg(args, uint32_t) << 32;
			modifier |= va_arg(args, uint32_t);
			assert(modifier == DRM_FORMAT_MOD_APPLE_GPU_TILED ||
			       modifier == DRM_FORMAT_MOD_LINEAR || modifier == DRM_FORMAT_MOD_INVALID);
			modifier_events++;
			if (modifier == DRM_FORMAT_MOD_INVALID) invalid_events[index]++;
		}
	} else {
		assert(!strcmp(wl_resource_get_class(resource), "zwp_linux_dmabuf_feedback_v1"));
		if (opcode == ZWP_LINUX_DMABUF_FEEDBACK_V1_FORMAT_TABLE) {
			int fd = va_arg(args, int);
			uint32_t size = va_arg(args, uint32_t);
			assert(size == 6 * sizeof(struct np_dmabuf_format_table_entry));
			struct np_dmabuf_format_table_entry entries[6];
			assert(pread(fd, entries, sizeof(entries), 0) == (ssize_t)sizeof(entries));
			assert((fcntl(fd, F_GET_SEALS) & F_SEAL_WRITE) != 0);
			for (unsigned i = 0; i < 6; i++)
				if (entries[i].modifier == DRM_FORMAT_MOD_INVALID) feedback_implicit++;
		} else if (opcode == ZWP_LINUX_DMABUF_FEEDBACK_V1_DONE) {
			feedback_done++;
		}
	}
	va_end(args);
}

static void reset_events(void)
{
	format_events = modifier_events = feedback_done = feedback_implicit = 0;
	invalid_events[0] = invalid_events[1] = 0;
}

int main(void)
{
	struct wl_display *display = wl_display_create();
	assert(display);
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
	struct wl_client *client = wl_client_create(display, sockets[0]);
	assert(client);
	struct np_dmabuf dmabuf = { .drm_fd = -1, .device = 123 };

	reset_events();
	dmabuf_bind(client, &dmabuf, 3, 2);
	assert(format_events == 2 && modifier_events == 6);
	assert(invalid_events[0] == 1 && invalid_events[1] == 1);
	wl_resource_destroy(wl_client_get_object(client, 2));

	reset_events();
	dmabuf_bind(client, &dmabuf, 2, 3);
	assert(format_events == 2 && modifier_events == 0);
	wl_resource_destroy(wl_client_get_object(client, 3));

	reset_events();
	dmabuf_bind(client, &dmabuf, 4, 4);
	struct wl_resource *resource = wl_client_get_object(client, 4);
	assert(resource && format_events == 0 && modifier_events == 0);
	get_default_feedback(client, resource, 5);
	assert(feedback_done == 1 && feedback_implicit == 2);
	assert(format_events == 0 && modifier_events == 0);
	wl_display_destroy_clients(display);
	wl_display_destroy(display);
	close(sockets[1]);
	puts("VM dmabuf v3 implicit modifier, v2 compatibility and v4 feedback isolation: PASS");
	return 0;
}
