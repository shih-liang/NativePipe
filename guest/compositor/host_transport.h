#ifndef NP_HOST_TRANSPORT_H
#define NP_HOST_TRANSPORT_H

#include <stdbool.h>

struct np_host;

/* Link-selected listener implementation.  Framing, bounded queues and parser
 * behavior remain shared in hostlink.c. */
bool np_host_transport_listen(struct np_host *host);
int np_host_transport_accept(struct np_host *host);
void np_host_set_nonblocking(int fd);
bool np_host_transport_connected(const struct np_host *host);
bool np_host_transport_prepare_input(struct np_host *host);

#endif
