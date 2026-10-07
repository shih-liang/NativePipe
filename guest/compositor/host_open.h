#ifndef NATIVEPIPE_HOST_OPEN_H
#define NATIVEPIPE_HOST_OPEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct np_server;
struct np_window_reader;

/* Remote --stdio sessions publish a private socket for np-open. The VM path
 * continues to use its guest-initiated vsock connection. */
bool np_host_open_init(struct np_server *server);
void np_host_open_finish(struct np_server *server);
bool np_host_open_response(struct np_server *server, uint32_t token,
                           const unsigned char *frame, size_t length);
/* A complete optional NPW2 command is handled even if its reply is malformed;
 * unrelated graphical commands on the same stream must remain usable. */
bool np_host_open_handle_command(struct np_server *server, struct np_window_reader *reader);

#endif
