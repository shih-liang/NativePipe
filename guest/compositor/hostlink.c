#define _GNU_SOURCE

#include "hostlink.h"

#include <errno.h>
#include <fcntl.h>
#ifdef NP_REMOTE
#include <netinet/in.h>
#else
#include <linux/vm_sockets.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NP_MAGIC "NPIP"
#define NP_VERSION 1
#define NP_HEADER 12
#define NP_MAX_PAYLOAD (8 * 1024 * 1024)
#ifdef NP_REMOTE
#define NP_REMOTE_HELLO_SIZE 16
#define NP_REMOTE_SURFACE_LANE 1
#endif

static void set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(fd, F_GETFD, 0);
	if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static uint64_t monotonic_millis(void) {
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

bool np_host_listen(struct np_host *host, uint32_t port) {
	memset(host, 0, sizeof(*host));
	host->listen_fd = -1;
	host->conn_fd = -1;
	host->port = port;

	int fd;
#ifdef NP_REMOTE
	fd = socket(AF_INET, SOCK_STREAM, 0);
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
#else
	fd = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "[wayland] vsock socket: %s\n", strerror(errno));
		return false;
	}
	struct sockaddr_vm addr;
	memset(&addr, 0, sizeof(addr));
	addr.svm_family = AF_VSOCK;
	addr.svm_cid = VMADDR_CID_ANY;
	addr.svm_port = port;
#endif
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[wayland] %s bind %u: %s\n",
#ifdef NP_REMOTE
		        "tcp",
#else
		        "vsock",
#endif
		        port, strerror(errno));
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
#ifdef NP_REMOTE
	fprintf(stderr, "[wayland] window channel listening on 127.0.0.1:%u\n", port);
#else
	fprintf(stderr, "[wayland] window channel listening on vsock port %u\n", port);
#endif
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
	if (host->listen_fd < 0) return;
	int fd = accept(host->listen_fd, NULL, NULL);
	if (fd < 0) return;
	if (host->conn_fd >= 0) {
#ifdef NP_REMOTE
		/* The surface lane is the admission lane. Replace a peer that has not
		 * completed NPRH, but once it has presented a valid token retain it while
		 * the matching media lane arrives. This prevents two simultaneous clients
		 * from continually crossing their surface/media sockets. Extra dials are
		 * still accepted and closed so the listener cannot spin. */
		if (host->remote_handshake_ready) {
			close(fd);
			return;
		}
#else
		close(fd);
		return;
#endif
		close(host->conn_fd);
	}
	set_nonblocking(fd);
	host->conn_fd = fd;
	host->buffer_len = 0;
	host->out_head = 0;
	host->out_len = 0;
	host->remote_hello_len = 0;
	host->remote_session_token = 0;
	host->remote_handshake_ready = false;
	host->input_enabled = false;
	host->remote_accept_millis = monotonic_millis();
#ifdef NP_REMOTE
	host->output_enabled = false;
#else
	host->output_enabled = true;
#endif
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
	host->remote_hello_len = 0;
	host->remote_session_token = 0;
	host->remote_handshake_ready = false;
	host->output_enabled = false;
	host->input_enabled = false;
	host->remote_accept_millis = 0;
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
#ifdef NP_REMOTE
	/* Until the surface and media sockets have presented the same nonce there
	 * is no session.  Discard live state here; the paired-session replay is the
	 * sole authoritative snapshot and channelReady must be its first frame. */
	if (!host->output_enabled) return false;
#endif
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
	return host->conn_fd >= 0;
}

void np_host_flush(struct np_host *host) {
	if (host->conn_fd < 0) return;
#ifdef NP_REMOTE
	if (!host->output_enabled) return;
#endif
	flush_outbound(host);
}

void np_host_set_output_enabled(struct np_host *host, bool enabled) {
	if (!host) return;
#ifdef NP_REMOTE
	host->output_enabled = enabled;
	if (!enabled) {
		host->out_head = 0;
		host->out_len = 0;
	}
#else
	(void)enabled;
	host->output_enabled = true;
#endif
}

void np_host_set_input_enabled(struct np_host *host, bool enabled) {
	if (!host) return;
#ifdef NP_REMOTE
	host->input_enabled = enabled;
#else
	(void)enabled;
	host->input_enabled = true;
#endif
}

bool np_host_unpaired_expired(const struct np_host *host,
                              uint64_t timeout_millis) {
#ifdef NP_REMOTE
	if (!host || host->conn_fd < 0 || host->output_enabled ||
	    host->remote_accept_millis == 0)
		return false;
	uint64_t now = monotonic_millis();
	return now >= host->remote_accept_millis &&
	       now - host->remote_accept_millis >= timeout_millis;
#else
	(void)host;
	(void)timeout_millis;
	return false;
#endif
}

uint64_t np_host_session_token(const struct np_host *host) {
	return host && host->remote_handshake_ready
		? host->remote_session_token : 0;
}

enum np_remote_pair_state np_remote_pair_tokens(uint64_t surface_token,
                                                uint64_t media_token) {
	if (surface_token == 0 || media_token == 0)
		return NP_REMOTE_PAIR_INCOMPLETE;
	return surface_token == media_token
		? NP_REMOTE_PAIR_MATCHED : NP_REMOTE_PAIR_MISMATCHED;
}

#ifdef NP_REMOTE
static bool pump_remote_handshake(struct np_host *host) {
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
#endif

void np_host_pump(struct np_host *host,
                  np_host_binary_handler binary_handler, void *user_data) {
	np_host_accept(host);
	if (host->conn_fd < 0) return;
#ifdef NP_REMOTE
	if (!host->remote_handshake_ready && !pump_remote_handshake(host)) return;
	/* A conforming host waits for channelReady before sending HostCommand. Do
	 * not leave early bytes unread on a level-triggered fd (that spins the
	 * compositor); reject the half-session instead. */
	if (!host->input_enabled) {
		unsigned char unexpected;
		ssize_t got = recv(host->conn_fd, &unexpected, 1, MSG_PEEK | MSG_DONTWAIT);
		if (got > 0) {
			fprintf(stderr, "[wayland] command arrived before channel pairing\n");
			np_host_disconnect(host);
		} else if (got == 0 ||
		           (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			np_host_disconnect(host);
		}
		return;
	}
#endif
	if (host->output_enabled) flush_outbound(host);
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
		if (!binary_handler || length < 4) {
			fprintf(stderr, "[wayland] invalid binary window payload\n");
			np_host_disconnect(host);
			return;
		}
		if (!binary_handler(payload, length, user_data)) {
			fprintf(stderr, "[wayland] malformed binary window payload\n");
			np_host_disconnect(host);
			return;
		}
		offset += NP_HEADER + length;
	}

	if (offset > 0) {
		memmove(host->buffer, host->buffer + offset, host->buffer_len - offset);
		host->buffer_len -= offset;
	}
}
