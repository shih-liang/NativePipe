// The window channel to the host, over vsock.
//
// Same framing as the guestd control channel — "NPIP", version, length, JSON —
// but a different port and no request/response wrapper: window traffic is a
// stream of events one way and commands the other.
//
// This carries metadata only. Pixels never travel here; a frame message names a
// virtio-gpu resource that is already host memory.

#ifndef NATIVEPIPE_HOSTLINK_H
#define NATIVEPIPE_HOSTLINK_H

#include <cjson/cJSON.h>
#include <stdbool.h>

/// NativePipePort.surface
#define NP_SURFACE_PORT 1025

/// Called for each command the host sends. `body` is the case's payload object.
typedef void (*np_host_handler)(const char *name, cJSON *body, void *user_data);
typedef void (*np_host_binary_handler)(const unsigned char *payload, size_t length,
                                       void *user_data);

struct np_host {
	int listen_fd;
	int conn_fd;
	unsigned char *buffer;
	size_t buffer_len;
	size_t buffer_cap;
	/// Outbound bytes not yet accepted by the socket. A non-blocking stream can
	/// take part of a frame, and writing the rest later is the only way to keep
	/// the framing intact.
	unsigned char *out;
	size_t out_len;
	size_t out_cap;
};

bool np_host_listen(struct np_host *host);
void np_host_finish(struct np_host *host);

/// Accepts a waiting host connection, if any. Non-blocking.
void np_host_accept(struct np_host *host);

/// Wraps `body` as {"name": body} and writes one frame. Takes ownership of body.
void np_host_send(struct np_host *host, const char *name, cJSON *body);

/// Drains readable frames and dispatches them. Non-blocking.
void np_host_pump(struct np_host *host, np_host_handler handler,
                  np_host_binary_handler binary_handler, void *user_data);

/// True when bytes are waiting for the socket to accept them. The event loop has
/// to watch for writability while this holds, or the backlog waits for an
/// unrelated wakeup that may never come.
static inline bool np_host_has_backlog(const struct np_host *host) {
	return host->out_len > 0;
}

/// Pushes as much of the backlog as the socket will take. Safe to call at any time.
void np_host_flush(struct np_host *host);

/// True once a host is attached; until then events are dropped rather than queued.
static inline bool np_host_connected(const struct np_host *host) {
	return host->conn_fd >= 0;
}

#endif
