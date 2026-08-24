#ifndef NATIVEPIPE_XDG_SHELL_H
#define NATIVEPIPE_XDG_SHELL_H

#include <stdint.h>

struct np_surface;
struct wl_client;

enum np_configure_state_bits {
	NP_CONFIGURE_MAXIMIZED = 1u << 0,
	NP_CONFIGURE_FULLSCREEN = 1u << 1,
	NP_CONFIGURE_RESIZING = 1u << 2,
	NP_CONFIGURE_ACTIVATED = 1u << 3,
};

void np_xdg_shell_bind(struct wl_client *client, void *data,
                       uint32_t version, uint32_t id);
void np_xdg_clear_configures(struct np_surface *surface);
void np_xdg_send_initial_role_configure(struct np_surface *surface);
void np_xdg_finish_toplevel_configure(struct np_surface *surface,
                                      uint32_t serial,
                                      uint32_t presentation_id);
void np_xdg_toplevel_configure_latched(struct np_surface *surface,
                                       uint32_t presentation_id);
void np_xdg_configure_toplevel_from_host(struct np_surface *surface,
                                         int32_t width, int32_t height,
                                         uint32_t state_bits);
void np_xdg_flush_pending_toplevel_configure(struct np_surface *surface);
void np_xdg_configure_popup(struct np_surface *surface,
                            int32_t x, int32_t y,
                            int32_t width, int32_t height,
                            uint32_t token);
void np_xdg_apply_popup_geometry(struct np_surface *surface,
                                 int32_t x, int32_t y,
                                 int32_t width, int32_t height);
void np_xdg_apply_size_constraints(struct np_surface *surface,
                                   int32_t minimum_width,
                                   int32_t minimum_height,
                                   int32_t maximum_width,
                                   int32_t maximum_height);

#endif
