// The RemotePipe window channel over loopback TCP (normally forwarded by SSH).
//
// NPIP provides versioned length framing around either JSON metadata or the
// high-rate binary messages. Encoded pixels travel on the separate NPEN media
// stream instead of this ordered state channel.

#ifndef NATIVEPIPE_HOSTLINK_H
#define NATIVEPIPE_HOSTLINK_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

/// RemotePipe TCP surface port.
#define NP_SURFACE_PORT 1025
/// Called for each command the host sends. `body` is the case's payload object.
typedef void (*np_host_handler)(const char *name, cJSON *body, void *user_data);
typedef void (*np_host_binary_handler)(const unsigned char *payload, size_t length,
                                       void *user_data);

struct np_host {
	int listen_fd;
	int conn_fd;
	uint32_t port;
	unsigned char *buffer;
	size_t buffer_len;
	size_t buffer_cap;
	/// Outbound bytes not yet accepted by the socket. A non-blocking stream can
	/// take part of a frame, and writing the rest later is the only way to keep
	/// the framing intact.
	unsigned char *out;
	size_t out_head;
	size_t out_len;
	size_t out_cap;
};

/// Listen on a loopback TCP port for remote / SSH-forwarded hosts.
bool np_host_listen_tcp(struct np_host *host, uint32_t port);
void np_host_finish(struct np_host *host);
/// Close only the current peer, retaining the listener for a clean reconnect.
void np_host_disconnect(struct np_host *host);

/// Accepts a waiting host connection, if any. Non-blocking.
void np_host_accept(struct np_host *host);

/// Wraps `body` as {"name": body} and writes one frame. Takes ownership of body.
void np_host_send(struct np_host *host, const char *name, cJSON *body);

/// Writes one already-encoded metadata payload inside NPIP framing. Returns
/// false when no host is attached or the bounded outbound queue cannot accept
/// the complete frame; callers then release any resources owned by that frame.
bool np_host_send_binary(struct np_host *host, const void *payload, size_t length);

/// Drains readable frames and dispatches them. Non-blocking.
void np_host_pump(struct np_host *host, np_host_handler handler,
                  np_host_binary_handler binary_handler, void *user_data);

/// True when bytes are waiting for the socket to accept them. The event loop has
/// to watch for writability while this holds, or the backlog waits for an
/// unrelated wakeup that may never come.
static inline bool np_host_has_backlog(const struct np_host *host) {
	return host->out_len > host->out_head;
}

/// Pushes as much of the backlog as the socket will take. Safe to call at any time.
void np_host_flush(struct np_host *host);

/// True once a host is attached; until then events are dropped rather than queued.
static inline bool np_host_connected(const struct np_host *host) {
	return host->conn_fd >= 0;
}

#endif
