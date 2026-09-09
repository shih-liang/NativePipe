#ifndef NATIVEPIPE_DATA_DEVICE_H
#define NATIVEPIPE_DATA_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-util.h>

struct np_server;
struct np_surface;
struct wl_client;
struct wl_resource;
struct np_window_reader;
bool np_data_handle_file_drag(struct np_server *server, struct np_window_reader *reader);

void np_data_device_manager_bind(struct wl_client *client, void *data,
                                 uint32_t version, uint32_t id);
void np_data_send_selection(struct np_server *server,
                            struct wl_resource *device);
void np_data_serve_host_request(struct np_server *server, uint32_t token,
                                const char *mime_type);
void np_data_deliver_host_data(struct np_server *server, uint32_t token,
                               const unsigned char *bytes, size_t length,
                               bool present);
void np_data_host_disconnected(struct np_server *server);
void np_data_drag_leave(struct np_server *server, uint32_t window_id);
void np_data_drag_enter(struct np_server *server, struct np_surface *surface,
                        wl_fixed_t x, wl_fixed_t y);
void np_data_drag_motion(struct np_server *server, struct np_surface *surface,
                         uint32_t time, wl_fixed_t x, wl_fixed_t y);
void np_data_drag_finish(struct np_server *server);

#endif
