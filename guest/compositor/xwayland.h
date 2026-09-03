#ifndef NATIVEPIPE_XWAYLAND_H
#define NATIVEPIPE_XWAYLAND_H

#include <stdbool.h>
struct np_server;
struct wl_client;

bool np_xwayland_init(struct np_server *server);
void np_xwayland_finish(struct np_server *server);
bool np_xwayland_owns_client(struct np_server *server,
                             struct wl_client *client);

#endif
