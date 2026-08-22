#ifndef NP_DECORATION_H
#define NP_DECORATION_H

struct np_server;
struct np_surface;
struct wl_display;

void np_decoration_advertise(struct wl_display *display, struct np_server *server);
void np_decoration_use_client_default(struct np_surface *surface);

#endif
