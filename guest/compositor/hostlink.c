#define _GNU_SOURCE

#include "hostlink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NP_MAGIC "NPIP"
#define NP_VERSION 1
#define NP_HEADER 12
#define NP_MAX_PAYLOAD (8 * 1024 * 1024)

static void set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(fd, F_GETFD, 0);
	if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

bool np_host_listen_tcp(struct np_host *host, uint32_t port) {
	memset(host, 0, sizeof(*host));
	host->listen_fd = -1;
	host->conn_fd = -1;
	host->port = port;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "[wayland] tcp socket: %s\n", strerror(errno));
		return false;
	}
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[wayland] tcp bind %u: %s\n", port, strerror(errno));
		close(fd);
		return false;
	}
	if (listen(fd, 1) < 0) {
		fprintf(stderr, "[wayland] tcp listen: %s\n", strerror(errno));
		close(fd);
		return false;
	}

	set_nonblocking(fd);
	host->listen_fd = fd;
	fprintf(stderr, "[wayland] window channel listening on 127.0.0.1:%u\n", port);
	return true;
}

void np_host_finish(struct np_host *host) {
	if (host->conn_fd >= 0) close(host->conn_fd);
	if (host->listen_fd >= 0) close(host->listen_fd);
	free(host->buffer);
	free(host->out);
	memset(host, 0, sizeof(*host));
	host->listen_fd = -1;
	host->conn_fd = -1;
}

void np_host_accept(struct np_host *host) {
	if (host->listen_fd < 0 || host->conn_fd >= 0) return;
	int fd = accept(host->listen_fd, NULL, NULL);
	if (fd < 0) return;
	set_nonblocking(fd);
	host->conn_fd = fd;
	host->buffer_len = 0;
	host->out_head = 0;
	host->out_len = 0;
	fprintf(stderr, "[wayland] host attached on port %u\n", host->port);
}

void np_host_disconnect(struct np_host *host) {
	bool was_connected = host->conn_fd >= 0;
	if (host->conn_fd >= 0) {
		close(host->conn_fd);
		host->conn_fd = -1;
	}
	host->buffer_len = 0;
	host->out_head = 0;
	host->out_len = 0;
	if (was_connected)
		fprintf(stderr, "[wayland] host detached from port %u\n", host->port);
}

/* One maximum-sized NPIP frame must always fit.  The queue cap is otherwise
 * smaller than the protocol cap and a valid large scene can never be sent. */
#define NP_MAX_OUTBOUND (NP_HEADER + NP_MAX_PAYLOAD)

/// Pushes as much of the outbound buffer as the socket will take.
static void flush_outbound(struct np_host *host) {
	while (host->out_head < host->out_len) {
		ssize_t written = send(
			host->conn_fd, host->out + host->out_head,
			host->out_len - host->out_head, MSG_NOSIGNAL);
		if (written > 0) {
			host->out_head += (size_t)written;
			continue;
		}
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
		np_host_disconnect(host);
		return;
	}
	host->out_head = 0;
	host->out_len = 0;
}

static bool reserve_outbound(struct np_host *host, size_t count) {
	size_t pending = host->out_len - host->out_head;
	if (count > NP_MAX_OUTBOUND - pending) return false;
	if (host->out_head && host->out_cap - host->out_len < count) {
		memmove(host->out, host->out + host->out_head, pending);
		host->out_head = 0;
		host->out_len = pending;
	}
	if (host->out_cap < host->out_len + count) {
		size_t cap = host->out_cap ? host->out_cap * 2 : 65536;
		while (cap < host->out_len + count) cap *= 2;
		unsigned char *grown = realloc(host->out, cap);
		if (!grown) return false;
		host->out = grown;
		host->out_cap = cap;
	}
	return true;
}

bool np_host_send_binary(struct np_host *host, const void *payload, size_t length) {
	if (!host || !payload || !length || length > NP_MAX_PAYLOAD ||
	    host->conn_fd < 0)
		return false;
	size_t frame_size = NP_HEADER + length;
	if (!reserve_outbound(host, frame_size)) {
		/* Structural and presentation messages are ordered state.  Dropping one
		 * would leave the two peers permanently divergent; reconnect instead so
		 * the compositor's normal replay sends one authoritative snapshot. */
		fprintf(stderr, "[wayland] window channel backlog full; reconnecting\n");
		np_host_disconnect(host);
		return false;
	}
	unsigned char header[NP_HEADER] = {
		'N', 'P', 'I', 'P', NP_VERSION, 0, 0, 0,
		(unsigned char)(length & 0xff),
		(unsigned char)((length >> 8) & 0xff),
		(unsigned char)((length >> 16) & 0xff),
		(unsigned char)((length >> 24) & 0xff),
	};
	memcpy(host->out + host->out_len, header, sizeof(header));
	memcpy(host->out + host->out_len + sizeof(header), payload, length);
	host->out_len += frame_size;
	flush_outbound(host);
	return true;
}

void np_host_send(struct np_host *host, const char *name, cJSON *body) {
	cJSON *envelope = cJSON_CreateObject();
	cJSON_AddItemToObject(envelope, name, body ? body : cJSON_CreateObject());
	char *payload = cJSON_PrintUnformatted(envelope);
	cJSON_Delete(envelope);
	if (!payload) return;

	// Dropping rather than queueing is deliberate: with no host attached there
	// is nothing to draw into, and a backlog of stale frames helps nobody.
	if (host->conn_fd < 0) {
		free(payload);
		return;
	}

	np_host_send_binary(host, payload, strlen(payload));
	free(payload);
}

void np_host_flush(struct np_host *host) {
	if (host->conn_fd < 0) return;
	flush_outbound(host);
}

void np_host_pump(struct np_host *host, np_host_handler handler,
                  np_host_binary_handler binary_handler, void *user_data) {
	np_host_accept(host);
	if (host->conn_fd < 0) return;
	flush_outbound(host);
	if (host->conn_fd < 0) return;

	for (;;) {
		/* Parse at most one protocol-sized frame per pump buffer.  More data can
		 * remain in the socket for the next level-triggered event; allowing the
		 * receive buffer to grow before validating its header lets a peer consume
		 * unbounded guest memory. */
		if (host->buffer_len >= NP_MAX_OUTBOUND) break;
		if (host->buffer_cap - host->buffer_len < 4096) {
			size_t cap = host->buffer_cap ? host->buffer_cap * 2 : 8192;
			if (cap > NP_MAX_OUTBOUND) cap = NP_MAX_OUTBOUND;
			unsigned char *grown = realloc(host->buffer, cap);
			if (!grown) return;
			host->buffer = grown;
			host->buffer_cap = cap;
		}

		size_t available = host->buffer_cap - host->buffer_len;
		if (available > NP_MAX_OUTBOUND - host->buffer_len)
			available = NP_MAX_OUTBOUND - host->buffer_len;
		ssize_t got = recv(host->conn_fd, host->buffer + host->buffer_len,
		                   available, 0);
		if (got == 0) {
			np_host_disconnect(host);
			return;
		}
		if (got < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			np_host_disconnect(host);
			return;
		}
		host->buffer_len += (size_t)got;
	}

	size_t offset = 0;
	while (host->buffer_len - offset >= NP_HEADER) {
		const unsigned char *frame = host->buffer + offset;
		if (memcmp(frame, NP_MAGIC, 4) != 0 || frame[4] != NP_VERSION) {
			fprintf(stderr, "[wayland] bad frame on the window channel\n");
			np_host_disconnect(host);
			return;
		}
		uint32_t length = (uint32_t)frame[8] | ((uint32_t)frame[9] << 8) |
		                  ((uint32_t)frame[10] << 16) | ((uint32_t)frame[11] << 24);
		if (length > NP_MAX_PAYLOAD) {
			np_host_disconnect(host);
			return;
		}
		if (host->buffer_len - offset < (size_t)NP_HEADER + length) break;

		const unsigned char *payload = frame + NP_HEADER;
		if (length >= 4 &&
		    (memcmp(payload, "NPMO", 4) == 0 ||
		     memcmp(payload, "NPSC", 4) == 0 ||
		     memcmp(payload, "NPCF", 4) == 0 ||
		     memcmp(payload, "NPPF", 4) == 0 ||
		     memcmp(payload, "NPFT", 4) == 0)) {
			if (binary_handler) binary_handler(payload, length, user_data);
		} else {
			cJSON *message = cJSON_ParseWithLength((const char *)payload, length);
			if (message) {
				cJSON *entry = message->child;  // single-key object, as Swift encodes it
				if (entry && entry->string) {
					handler(entry->string, entry, user_data);
				}
				cJSON_Delete(message);
			}
		}
		offset += NP_HEADER + length;
	}

	if (offset > 0) {
		memmove(host->buffer, host->buffer + offset, host->buffer_len - offset);
		host->buffer_len -= offset;
	}
}
