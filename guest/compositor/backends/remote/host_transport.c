#include "host_transport.h"

#include "hostlink.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NP_REMOTE_HELLO_SIZE 16
#define NP_REMOTE_SURFACE_LANE 1

bool np_host_transport_listen(struct np_host *host)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "[wayland] tcp socket: %s\n", strerror(errno));
		return false;
	}
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons((uint16_t)host->port);
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    listen(fd, 1) < 0) {
		fprintf(stderr, "[wayland] tcp listen %u: %s\n",
		        host->port, strerror(errno));
		close(fd);
		return false;
	}
	np_host_set_nonblocking(fd);
	host->listen_fd = fd;
	host->requires_handshake = true;
	host->replace_unready_peer = true;
	fprintf(stderr, "[wayland] window channel listening on 127.0.0.1:%u\n",
	        host->port);
	return true;
}

bool np_host_transport_connected(const struct np_host *host)
{
	return host && host->conn_fd >= 0 && host->remote_handshake_ready;
}

static bool pump_remote_handshake(struct np_host *host)
{
	while (host->remote_hello_len < NP_REMOTE_HELLO_SIZE) {
		ssize_t got = recv(
			host->conn_fd, host->remote_hello + host->remote_hello_len,
			NP_REMOTE_HELLO_SIZE - host->remote_hello_len, 0);
		if (got > 0) {
			host->remote_hello_len += (size_t)got;
			continue;
		}
		if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return false;
		np_host_disconnect(host);
		return false;
	}
	const unsigned char *hello = host->remote_hello;
	if (memcmp(hello, "NPRH", 4) != 0 || hello[4] != 1 ||
	    hello[5] != NP_REMOTE_SURFACE_LANE || hello[6] != 0 || hello[7] != 0) {
		fprintf(stderr, "[wayland] invalid remote surface handshake\n");
		np_host_disconnect(host);
		return false;
	}
	uint64_t token = 0;
	for (unsigned int index = 0; index < 8; index++)
		token |= (uint64_t)hello[8 + index] << (index * 8);
	if (token == 0) {
		np_host_disconnect(host);
		return false;
	}
	host->remote_session_token = token;
	host->remote_handshake_ready = true;
	return true;
}

bool np_host_transport_prepare_input(struct np_host *host)
{
	if (!host || host->conn_fd < 0) return false;
	if (!host->remote_handshake_ready && !pump_remote_handshake(host)) return false;
	/* A conforming host waits for channelReady before sending HostCommand. Do
	 * not leave early bytes unread on a level-triggered fd. */
	if (!host->input_enabled) {
		unsigned char unexpected;
		ssize_t got = recv(
			host->conn_fd, &unexpected, 1, MSG_PEEK | MSG_DONTWAIT);
		if (got > 0) {
			fprintf(stderr, "[wayland] command arrived before channel pairing\n");
			np_host_disconnect(host);
		} else if (got == 0 ||
		           (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			np_host_disconnect(host);
		}
		return false;
	}
	return true;
}
