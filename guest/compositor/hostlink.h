// Shared NPIP binary framing for RemotePipe TCP and VMPipe vsock transports.

#ifndef NATIVEPIPE_HOSTLINK_H
#define NATIVEPIPE_HOSTLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NP_SURFACE_PORT 1025
#define NP_WINDOW_EVENT_PORT 4096
#define NP_WINDOW_CONTROL_PORT 4097
#define NP_WINDOW_INPUT_PORT 4098
#define NP_WINDOW_FEEDBACK_PORT 4099

typedef bool (*np_host_binary_handler)(const unsigned char *payload, size_t length,
                                       void *user_data);

struct np_host {
	int listen_fd;
	int conn_fd;
	uint32_t port;
	unsigned char *buffer;
	size_t buffer_len;
	size_t buffer_cap;
	unsigned char *out;
	size_t out_head;
	size_t out_len;
	size_t out_cap;
	unsigned char remote_hello[16];
	size_t remote_hello_len;
	uint64_t remote_session_token;
	bool remote_handshake_ready;
	bool requires_handshake;
	bool replace_unready_peer;
	bool output_enabled;
	bool input_enabled;
	uint64_t remote_accept_millis;
};

bool np_host_listen(struct np_host *host, uint32_t port);
void np_host_finish(struct np_host *host);
void np_host_disconnect(struct np_host *host);
void np_host_accept(struct np_host *host);
bool np_host_send_binary(struct np_host *host, const void *payload, size_t length);
void np_host_pump(struct np_host *host,
                  np_host_binary_handler binary_handler, void *user_data);

static inline bool np_host_has_backlog(const struct np_host *host)
{
	return host->out_len > host->out_head;
}

void np_host_flush(struct np_host *host);
uint64_t np_host_session_token(const struct np_host *host);
/// Gate normal NPIP output until both RemotePipe lanes belong to one session.
/// VMPipe does not use this gate.
void np_host_set_output_enabled(struct np_host *host, bool enabled);
void np_host_set_input_enabled(struct np_host *host, bool enabled);
bool np_host_unpaired_expired(const struct np_host *host, uint64_t timeout_millis);

bool np_host_connected(const struct np_host *host);

#endif
