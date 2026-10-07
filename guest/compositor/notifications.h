#ifndef NATIVEPIPE_NOTIFICATIONS_H
#define NATIVEPIPE_NOTIFICATIONS_H

#include <stdbool.h>
#include <stdint.h>

struct np_server;
struct np_window_reader;

/* Serves org.freedesktop.Notifications for this session and forwards what
 * applications post to the host, on the window protocol. Without a session bus,
 * or when another daemon owns the name, initialization returns false without
 * preventing the graphical session from starting. */
bool np_notifications_init(struct np_server *server);
void np_notifications_finish(struct np_server *server);
/* Retire the old host session's IDs and pending posts before a reconnect. */
void np_notifications_reset(struct np_server *server);

/* Host to guest: the user closed, or acted on, a notification on the Mac. */
/* Optional notification content is refused within its already-framed command;
 * malformed text cannot tear down the authoritative window channel. */
bool np_notifications_handle_command(struct np_server *server, struct np_window_reader *reader);

#endif
