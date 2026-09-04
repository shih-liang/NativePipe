#ifndef NATIVEPIPE_MEDIALINK_H
#define NATIVEPIPE_MEDIALINK_H

#include "media.h"

#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

struct np_media_frame;

struct np_media {
	int listen_fd;
	int conn_fd;
	bool just_attached;
	uint8_t remote_hello[16];
	size_t remote_hello_len;
	uint64_t remote_session_token;
	bool remote_handshake_ready;
	bool session_ready;
	pthread_mutex_t lock;
	bool lock_initialized;
	pthread_cond_t writer_cond;
	pthread_t writer_thread;
	bool writer_started;
	bool writer_stopping;
	uint64_t connection_generation;
	struct np_media_frame *writer_head;
	struct np_media_frame *writer_tail;
	size_t writer_queued_bytes;
	size_t writer_inflight_bytes;
	size_t writer_reserved_bytes;
};

/// Initialize media state and its bounded serial writer without opening a
/// listener. Production uses this through np_media_listen; transport tests use
/// it with a socketpair.
bool np_media_init(struct np_media *media);
bool np_media_listen(struct np_media *media);
void np_media_finish(struct np_media *media);
void np_media_accept(struct np_media *media);
/// Drop a half-closed peer so a new client can attach.
void np_media_pump(struct np_media *media);
bool np_media_connected(struct np_media *media);
uint64_t np_media_session_token(struct np_media *media);
void np_media_set_session_ready(struct np_media *media, bool ready);
void np_media_connection_identity(struct np_media *media, int *fd,
                                  uint64_t *generation);
void np_media_disconnect(struct np_media *media);
bool np_media_take_just_attached(struct np_media *media);

/// Copy and enqueue one NPEN frame. The serial writer preserves call order and
/// fails the current lane closed if the bounded queue cannot accept the frame.
bool np_media_send(struct np_media *media, uint8_t codec, uint8_t flags,
                   uint32_t surface_id,
                   uint32_t resource_id,
                   uint16_t width, uint16_t height, uint64_t pts_ns,
                   uint16_t epoch, const uint8_t *payload, uint32_t length);

#endif
