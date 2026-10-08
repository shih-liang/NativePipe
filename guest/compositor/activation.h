#ifndef NP_ACTIVATION_H
#define NP_ACTIVATION_H

#include <stdbool.h>
#include <stdint.h>

struct np_server;
struct np_surface;

/* Record only actual key/button presses delivered to a client. Focus and
 * motion do not authorize a client to activate another window. */
#define NP_ACTIVATION_TIMEOUT_MS 5000u

bool np_activation_advertise(struct np_server *server);
void np_activation_record_input(struct np_server *server,
	                           struct np_surface *surface, uint32_t serial);
void np_activation_revoke_surface(struct np_surface *surface);

#endif
