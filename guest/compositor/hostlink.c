#define _GNU_SOURCE

#include "hostlink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/vm_sockets.h>
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
}

bool np_host_listen(struct np_host *host) {
	memset(host, 0, sizeof(*host));
	host->listen_fd = -1;
	host->conn_fd = -1;
	host->transport = NP_HOST_VSOCK;

	int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "[wayland] vsock socket: %s\n", strerror(errno));
		return false;
	}

	struct sockaddr_vm addr;
	memset(&addr, 0, sizeof(addr));
	addr.svm_family = AF_VSOCK;
	addr.svm_cid = VMADDR_CID_ANY;
	addr.svm_port = NP_SURFACE_PORT;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[wayland] vsock bind %d: %s\n", NP_SURFACE_PORT, strerror(errno));
		close(fd);
		return false;
	}
	if (listen(fd, 1) < 0) {
		fprintf(stderr, "[wayland] vsock listen: %s\n", strerror(errno));
		close(fd);
		return false;
	}

	set_nonblocking(fd);
	host->listen_fd = fd;
	fprintf(stderr, "[wayland] window channel listening on vsock port %d\n", NP_SURFACE_PORT);
	return true;
}

bool np_host_listen_tcp(struct np_host *host) {
	memset(host, 0, sizeof(*host));
	host->listen_fd = -1;
	host->conn_fd = -1;
	host->transport = NP_HOST_TCP;

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
	addr.sin_port = htons(NP_SURFACE_PORT);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[wayland] tcp bind %d: %s\n", NP_SURFACE_PORT, strerror(errno));
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
	fprintf(stderr, "[wayland] window channel listening on 127.0.0.1:%d\n", NP_SURFACE_PORT);
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
	host->out_len = 0;
	fprintf(stderr, "[wayland] host attached to the window channel\n");
}

static void drop_connection(struct np_host *host) {
	if (host->conn_fd >= 0) {
		close(host->conn_fd);
		host->conn_fd = -1;
	}
	host->buffer_len = 0;
	host->out_len = 0;
	fprintf(stderr, "[wayland] host detached\n");
}

/// Beyond this the host is not draining and the backlog is stale anyway.
#define NP_MAX_OUTBOUND (4u * 1024u * 1024u)

/// Pushes as much of the outbound buffer as the socket will take.
static void flush_outbound(struct np_host *host) {
	while (host->out_len > 0) {
		ssize_t written = send(host->conn_fd, host->out, host->out_len, MSG_NOSIGNAL);
		if (written > 0) {
			memmove(host->out, host->out + written, host->out_len - (size_t)written);
			host->out_len -= (size_t)written;
			continue;
		}
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
		drop_connection(host);
		return;
	}
}

static bool queue_outbound(struct np_host *host, const unsigned char *bytes, size_t count) {
	if (host->out_len + count > NP_MAX_OUTBOUND) return false;
	if (host->out_cap < host->out_len + count) {
		size_t cap = host->out_cap ? host->out_cap * 2 : 65536;
		while (cap < host->out_len + count) cap *= 2;
		unsigned char *grown = realloc(host->out, cap);
		if (!grown) return false;
		host->out = grown;
		host->out_cap = cap;
	}
	memcpy(host->out + host->out_len, bytes, count);
	host->out_len += count;
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

	size_t length = strlen(payload);
	unsigned char header[NP_HEADER];
	memcpy(header, NP_MAGIC, 4);
	header[4] = NP_VERSION;
	header[5] = header[6] = header[7] = 0;
	header[8] = (unsigned char)(length & 0xff);
	header[9] = (unsigned char)((length >> 8) & 0xff);
	header[10] = (unsigned char)((length >> 16) & 0xff);
	header[11] = (unsigned char)((length >> 24) & 0xff);

	// Queue whole frames, never partial ones: a short write on a non-blocking
	// socket would splice two frames together and desynchronise the reader.
	//
	// When the backlog is full the *new* frame is dropped. Clearing the buffer
	// instead — which is what this did — truncates whatever frame was half
	// written, and the reader then sees a bad magic and drops the connection.
	if (host->out_len + NP_HEADER + length > NP_MAX_OUTBOUND) {
		fprintf(stderr, "[wayland] window channel backlog full; dropping one event\n");
		free(payload);
		return;
	}
	if (queue_outbound(host, header, NP_HEADER)) {
		queue_outbound(host, (const unsigned char *)payload, length);
		flush_outbound(host);
	}
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
		if (host->buffer_cap - host->buffer_len < 4096) {
			size_t cap = host->buffer_cap ? host->buffer_cap * 2 : 8192;
			unsigned char *grown = realloc(host->buffer, cap);
			if (!grown) return;
			host->buffer = grown;
			host->buffer_cap = cap;
		}

		ssize_t got = recv(host->conn_fd, host->buffer + host->buffer_len,
		                   host->buffer_cap - host->buffer_len, 0);
		if (got == 0) {
			drop_connection(host);
			return;
		}
		if (got < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			drop_connection(host);
			return;
		}
		host->buffer_len += (size_t)got;
	}

	size_t offset = 0;
	while (host->buffer_len - offset >= NP_HEADER) {
		const unsigned char *frame = host->buffer + offset;
		if (memcmp(frame, NP_MAGIC, 4) != 0 || frame[4] != NP_VERSION) {
			fprintf(stderr, "[wayland] bad frame on the window channel\n");
			drop_connection(host);
			return;
		}
		uint32_t length = (uint32_t)frame[8] | ((uint32_t)frame[9] << 8) |
		                  ((uint32_t)frame[10] << 16) | ((uint32_t)frame[11] << 24);
		if (length > NP_MAX_PAYLOAD) {
			drop_connection(host);
			return;
		}
		if (host->buffer_len - offset < (size_t)NP_HEADER + length) break;

		const unsigned char *payload = frame + NP_HEADER;
		if (length >= 4 &&
		    (memcmp(payload, "NPMO", 4) == 0 || memcmp(payload, "NPSC", 4) == 0)) {
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
