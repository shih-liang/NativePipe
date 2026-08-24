#ifndef NP_SYNCOBJ_H
#define NP_SYNCOBJ_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct np_surface;
struct np_server;
struct np_sync_point;

void np_syncobj_advertise(struct wl_display *display, struct np_server *server);
void np_syncobj_surface_destroyed(struct np_surface *surface);
bool np_syncobj_has_pending(struct np_surface *surface);

/* Validate and consume the explicit-sync state belonging to one wl_surface
 * commit. Points returned here own timeline references. */
bool np_syncobj_take_commit(
	struct np_surface *surface, bool buffer_set, struct wl_resource *buffer,
	struct np_sync_point **acquire, struct np_sync_point **release);

bool np_sync_point_ready(struct np_sync_point *point);
/* Returns a pollable eventfd which signals when the timeline point is ready. */
int np_sync_point_wait_fd(struct np_sync_point *point);
void np_sync_point_destroy(struct np_sync_point *point);
void np_sync_point_signal(struct np_sync_point *point);

#endif
