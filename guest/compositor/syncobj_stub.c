#include "syncobj.h"

/* RemotePipe does not advertise linux-drm-syncobj. Keep the shared Wayland
 * state machine explicit about that absence instead of linking DRM ioctls into
 * the remote compositor. */
void np_syncobj_advertise(struct wl_display *display, struct np_server *server)
{
	(void)display;
	(void)server;
}

void np_syncobj_surface_destroyed(struct np_surface *surface) { (void)surface; }
bool np_syncobj_has_pending(struct np_surface *surface)
{
	(void)surface;
	return false;
}

bool np_syncobj_take_commit(
	struct np_surface *surface, bool buffer_set, struct wl_resource *buffer,
	struct np_sync_point **acquire, struct np_sync_point **release)
{
	(void)surface;
	(void)buffer_set;
	(void)buffer;
	if (acquire) *acquire = NULL;
	if (release) *release = NULL;
	return true;
}

bool np_sync_point_ready(struct np_sync_point *point)
{
	(void)point;
	return true;
}
int np_sync_point_wait_fd(struct np_sync_point *point) { (void)point; return -1; }
void np_sync_point_destroy(struct np_sync_point *point) { (void)point; }
void np_sync_point_signal(struct np_sync_point *point) { (void)point; }
