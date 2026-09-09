#include "host_transport.h"
#include "hostlink.h"
#include <unistd.h>

int np_host_transport_accept(struct np_host *host)
{
    (void)host;
    return -1; /* SSH supplies stdin; remote mode never accepts a socket. */
}

/* SSH owns the authenticated connection; there are no listening sockets. */
bool np_host_transport_listen(struct np_host *host)
{
    host->conn_fd = dup(STDIN_FILENO);
    if (host->conn_fd < 0) return false;
    np_host_set_nonblocking(host->conn_fd);
    host->input_enabled = true;
    return true;
}
bool np_host_transport_connected(const struct np_host *host)
{
    return host && host->conn_fd >= 0;
}
bool np_host_transport_prepare_input(struct np_host *host)
{
    return np_host_transport_connected(host);
}
