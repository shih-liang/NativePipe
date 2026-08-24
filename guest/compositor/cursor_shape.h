#ifndef NP_CURSOR_SHAPE_H
#define NP_CURSOR_SHAPE_H

struct wl_display;
struct np_server;

void np_cursor_shape_advertise(struct wl_display *display,
                               struct np_server *server);

#endif
