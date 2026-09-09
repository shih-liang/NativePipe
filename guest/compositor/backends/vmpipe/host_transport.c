#define _GNU_SOURCE
#include "host_transport.h"

#include "hostlink.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <unistd.h>

bool np_host_transport_listen(struct np_host *host)
{
	int fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		fprintf(stderr, "[wayland] vsock socket: %s\n", strerror(errno));
		return false;
	}
	struct sockaddr_vm address;
	memset(&address, 0, sizeof(address));
	address.svm_family = AF_VSOCK;
	address.svm_cid = VMADDR_CID_ANY;
	address.svm_port = host->port;
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    listen(fd, 1) < 0) {
		fprintf(stderr, "[wayland] vsock listen %u: %s\n",
		        host->port, strerror(errno));
		close(fd);
		return false;
	}
	np_host_set_nonblocking(fd);
	host->listen_fd = fd;
	fprintf(stderr, "[wayland] window channel listening on vsock port %u\n",
	        host->port);
	return true;
}

bool np_host_transport_connected(const struct np_host *host)
{
	return host && host->conn_fd >= 0;
}

int np_host_transport_accept(struct np_host *host)
{
    struct sockaddr_vm peer = {0};
    socklen_t size = sizeof(peer);
    int fd = accept4(host->listen_fd, (struct sockaddr *)&peer, &size,
                     SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd >= 0 && (size != sizeof(peer) || peer.svm_family != AF_VSOCK ||
                    peer.svm_cid != VMADDR_CID_HOST)) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}

bool np_host_transport_prepare_input(struct np_host *host)
{
	return host && host->conn_fd >= 0;
}
