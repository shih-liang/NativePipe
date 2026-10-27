#ifndef NP_ACCESSIBILITY_H
#define NP_ACCESSIBILITY_H
#include <stdbool.h>
#include <stddef.h>
struct np_server;
/* Optional: without a session bus agents fall back to screenshots. */
bool np_accessibility_init(struct np_server *server);
void np_accessibility_finish(struct np_server *server);
bool np_accessibility_handle_command(struct np_server *server, const unsigned char *payload, size_t length);
#endif
