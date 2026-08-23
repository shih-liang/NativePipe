// The window channel to the host, over vsock or TCP.
//
// NPIP provides versioned length framing around either JSON metadata or the
// high-rate binary messages. Local VM events use 1025; host control, input and
// frame feedback each use their own unidirectional port below. RemotePipe keeps
// its legacy bidirectional TCP stream on 1025.
//
// This carries metadata only. Local VM pixels never travel here — a frame
// message names a virtio-gpu resource that is already host memory. Remote
// display sends compressed pixels on NativePipePort.media (NPEN) instead.

#ifndef NATIVEPIPE_HOSTLINK_H
#define NATIVEPIPE_HOSTLINK_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

/// NativePipePort.surface
#define NP_SURFACE_PORT 1025
#define NP_WINDOW_CONTROL_PORT 1286
#define NP_WINDOW_INPUT_PORT 1287
#define NP_WINDOW_FEEDBACK_PORT 1288

enum np_host_transport {
	NP_HOST_VSOCK = 0,
	NP_HOST_TCP = 1,
};

/// Called for each command the host sends. `body` is the case's payload object.
typedef void (*np_host_handler)(const char *name, cJSON *body, void *user_data);
typedef void (*np_host_binary_handler)(const unsigned char *payload, size_t length,
                                       void *user_data);

struct np_host {
	int listen_fd;
	int conn_fd;
	uint32_t port;
	enum np_host_transport transport;
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

bool np_host_listen(struct np_host *host, uint32_t port);
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
	return host->out_len > 0;
}

/// Pushes as much of the backlog as the socket will take. Safe to call at any time.
void np_host_flush(struct np_host *host);

/// True once a host is attached; until then events are dropped rather than queued.
static inline bool np_host_connected(const struct np_host *host) {
	return host->conn_fd >= 0;
}

#endif
