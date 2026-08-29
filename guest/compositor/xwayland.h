#ifndef NATIVEPIPE_XWAYLAND_H
#define NATIVEPIPE_XWAYLAND_H

#include <stdbool.h>
#include <stdint.h>

struct np_server;
struct np_surface;

#ifndef NP_REMOTE
bool np_xwayland_init(struct np_server *server);
void np_xwayland_finish(struct np_server *server);
void np_xwayland_surface_created(struct np_server *server,
                                 struct np_surface *surface);
void np_xwayland_surface_destroyed(struct np_server *server,
                                   struct np_surface *surface);
void np_xwayland_configure(struct np_surface *surface,
                           int32_t width, int32_t height,
                           uint32_t state_bits);
void np_xwayland_set_focus(struct np_server *server,
                           struct np_surface *surface);
void np_xwayland_close(struct np_surface *surface);
void np_xwayland_force_quit(struct np_surface *surface);
void np_xwayland_dismiss_popup(struct np_surface *surface);
#else
static inline bool np_xwayland_init(struct np_server *server)
{ (void)server; return true; }
static inline void np_xwayland_finish(struct np_server *server) { (void)server; }
static inline void np_xwayland_surface_created(
    struct np_server *server, struct np_surface *surface)
{ (void)server; (void)surface; }
static inline void np_xwayland_surface_destroyed(
    struct np_server *server, struct np_surface *surface)
{ (void)server; (void)surface; }
static inline void np_xwayland_configure(
    struct np_surface *surface, int32_t width, int32_t height, uint32_t state_bits)
{ (void)surface; (void)width; (void)height; (void)state_bits; }
static inline void np_xwayland_set_focus(
    struct np_server *server, struct np_surface *surface)
{ (void)server; (void)surface; }
static inline void np_xwayland_close(struct np_surface *surface) { (void)surface; }
static inline void np_xwayland_force_quit(struct np_surface *surface) { (void)surface; }
static inline void np_xwayland_dismiss_popup(struct np_surface *surface) { (void)surface; }
#endif

#endif
