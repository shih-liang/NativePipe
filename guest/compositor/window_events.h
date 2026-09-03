#ifndef NATIVEPIPE_WINDOW_EVENTS_H
#define NATIVEPIPE_WINDOW_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "windowwire.h"

struct np_server;

bool np_window_event_send_message(struct np_server *server,
                                  struct np_window_message *message);
/* Fixed-size, loss-intolerant guest lifecycle records on the NPW2 wire. */
bool np_window_event_send(struct np_server *server, uint8_t opcode,
                          const uint32_t *values, size_t count);
bool np_window_event_send_force_quit_capability(
    struct np_server *server, uint32_t window, bool supported);
bool np_window_event_send_popup_placement(
    struct np_server *server, uint32_t window, uint32_t parent,
    int32_t x, int32_t y, int32_t flip_x, int32_t flip_y,
    int32_t width, int32_t height, uint32_t adjustment,
    uint32_t token, bool reactive);
bool np_window_event_send_frame(struct np_server *server, uint32_t surface,
                                const struct np_window_frame *frame,
                                unsigned char **bytes, size_t *size);

#endif
