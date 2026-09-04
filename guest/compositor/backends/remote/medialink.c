#include "medialink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define NP_REMOTE_HELLO_SIZE 16
#define NP_REMOTE_MEDIA_LANE 2
#define NP_MEDIA_MAX_PAYLOAD (32u * 1024u * 1024u)
#define NP_MEDIA_WRITER_LIMIT \
	(2u * ((size_t)NP_MEDIA_MAX_PAYLOAD + NP_MEDIA_HEADER_SIZE))

struct np_media_frame {
	struct np_media_frame *next;
	uint64_t generation;
	size_t size;
	uint8_t bytes[];
};

static void free_frames(struct np_media_frame *frame) {
	while (frame) {
		struct np_media_frame *next = frame->next;
		free(frame);
		frame = next;
	}
}

static struct np_media_frame *detach_queued_frames_locked(
	struct np_media *media) {
	struct np_media_frame *frames = media->writer_head;
	media->writer_head = NULL;
	media->writer_tail = NULL;
	media->writer_queued_bytes = 0;
	return frames;
}

/* The shutdown reaches any descriptor duplicated by the writer and wakes a
 * blocked poll/send. Advancing the generation prevents that writer from
 * closing or publishing failure into a subsequently accepted connection. */
static struct np_media_frame *close_connection_locked(struct np_media *media) {
	struct np_media_frame *frames = detach_queued_frames_locked(media);
	if (media->conn_fd >= 0) {
		shutdown(media->conn_fd, SHUT_RDWR);
		close(media->conn_fd);
	}
	media->conn_fd = -1;
	media->just_attached = false;
	media->remote_hello_len = 0;
	media->remote_session_token = 0;
	media->remote_handshake_ready = false;
	media->session_ready = false;
	media->connection_generation++;
	pthread_cond_broadcast(&media->writer_cond);
	return frames;
}

static bool writer_generation_active(struct np_media *media,
	                              uint64_t generation) {
	pthread_mutex_lock(&media->lock);
	bool active = !media->writer_stopping && media->conn_fd >= 0 &&
	              media->connection_generation == generation;
	pthread_mutex_unlock(&media->lock);
	return active;
}

static bool write_all(struct np_media *media, uint64_t generation, int fd,
	              const void *buf, size_t len) {
	const uint8_t *p = buf;
	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n > 0) {
			p += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd out = { .fd = fd, .events = POLLOUT };
			int ready;
			do ready = poll(&out, 1, 50); while (ready < 0 && errno == EINTR);
			if (ready < 0 || (ready > 0 &&
			    (out.revents & (POLLERR | POLLHUP | POLLNVAL))))
				return false;
			if (!writer_generation_active(media, generation)) return false;
			continue;
		}
		return false;
	}
	return true;
}

static void *media_writer_main(void *opaque) {
	struct np_media *media = opaque;
	for (;;) {
		pthread_mutex_lock(&media->lock);
		while (!media->writer_stopping && !media->writer_head)
			pthread_cond_wait(&media->writer_cond, &media->lock);
		if (media->writer_stopping) {
			pthread_mutex_unlock(&media->lock);
			break;
		}

		struct np_media_frame *frame = media->writer_head;
		media->writer_head = frame->next;
		if (!media->writer_head) media->writer_tail = NULL;
		media->writer_queued_bytes -= frame->size;
		media->writer_inflight_bytes += frame->size;

		bool current = media->conn_fd >= 0 && media->session_ready &&
		               media->remote_handshake_ready &&
		               media->connection_generation == frame->generation;
		int fd = current ? dup(media->conn_fd) : -1;
		if (fd >= 0) {
			int flags = fcntl(fd, F_GETFD, 0);
			if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
		}
		struct np_media_frame *discarded = NULL;
		if (current && fd < 0)
			discarded = close_connection_locked(media);
		pthread_mutex_unlock(&media->lock);
		free_frames(discarded);

		bool written = fd >= 0 && write_all(
			media, frame->generation, fd, frame->bytes, frame->size);
		if (fd >= 0) close(fd);

		pthread_mutex_lock(&media->lock);
		media->writer_inflight_bytes -= frame->size;
		discarded = NULL;
		if (!written && media->connection_generation == frame->generation &&
		    media->conn_fd >= 0) {
			discarded = close_connection_locked(media);
			fprintf(stderr, "[media] client detached\n");
		}
		pthread_mutex_unlock(&media->lock);
		free(frame);
		free_frames(discarded);
	}
	return NULL;
}

static void set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(fd, F_GETFD, 0);
	if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

bool np_media_init(struct np_media *media) {
	if (!media) return false;
	memset(media, 0, sizeof(*media));
	media->listen_fd = -1;
	media->conn_fd = -1;
	if (pthread_mutex_init(&media->lock, NULL) != 0) return false;
	if (pthread_cond_init(&media->writer_cond, NULL) != 0) {
		pthread_mutex_destroy(&media->lock);
		return false;
	}
	media->lock_initialized = true;
	media->connection_generation = 1;
	if (pthread_create(
		    &media->writer_thread, NULL, media_writer_main, media) != 0) {
		media->lock_initialized = false;
		pthread_cond_destroy(&media->writer_cond);
		pthread_mutex_destroy(&media->lock);
		return false;
	}
	media->writer_started = true;
	return true;
}

bool np_media_listen(struct np_media *media) {
	if (!np_media_init(media)) return false;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		np_media_finish(media);
		return false;
	}
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(NP_MEDIA_PORT);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 1) < 0) {
		fprintf(stderr, "[media] bind/listen %d: %s\n", NP_MEDIA_PORT, strerror(errno));
		close(fd);
		np_media_finish(media);
		return false;
	}
	set_nonblocking(fd);
	media->listen_fd = fd;
	fprintf(stderr, "[media] listening on 127.0.0.1:%d\n", NP_MEDIA_PORT);
	return true;
}

void np_media_finish(struct np_media *media) {
	if (!media || !media->lock_initialized) return;
	pthread_mutex_lock(&media->lock);
	media->writer_stopping = true;
	struct np_media_frame *discarded = close_connection_locked(media);
	if (media->listen_fd >= 0) close(media->listen_fd);
	media->listen_fd = -1;
	pthread_cond_broadcast(&media->writer_cond);
	pthread_mutex_unlock(&media->lock);
	free_frames(discarded);
	if (media->writer_started) {
		pthread_join(media->writer_thread, NULL);
		media->writer_started = false;
	}
	pthread_cond_destroy(&media->writer_cond);
	pthread_mutex_destroy(&media->lock);
	media->lock_initialized = false;
}

void np_media_accept(struct np_media *media) {
	if (!media || !media->lock_initialized) return;
	pthread_mutex_lock(&media->lock);
	if (media->listen_fd < 0) {
		pthread_mutex_unlock(&media->lock);
		return;
	}
	int fd = accept(media->listen_fd, NULL, NULL);
	if (fd < 0) {
		pthread_mutex_unlock(&media->lock);
		return;
	}
	struct np_media_frame *discarded = NULL;
	if (media->conn_fd >= 0) {
		/* See np_host_accept: replace only an unpaired lane. */
		if (media->session_ready) {
			close(fd);
			pthread_mutex_unlock(&media->lock);
			return;
		}
		discarded = close_connection_locked(media);
	}
	set_nonblocking(fd);
	media->connection_generation++;
	media->conn_fd = fd;
	media->just_attached = false;
	media->remote_hello_len = 0;
	media->remote_session_token = 0;
	media->remote_handshake_ready = false;
	media->session_ready = false;
	fprintf(stderr, "[media] client attached\n");
	pthread_mutex_unlock(&media->lock);
	free_frames(discarded);
}

void np_media_pump(struct np_media *media) {
	if (!media || !media->lock_initialized) return;
	pthread_mutex_lock(&media->lock);
	if (media->conn_fd < 0) {
		pthread_mutex_unlock(&media->lock);
		return;
	}
	while (!media->remote_handshake_ready &&
	       media->remote_hello_len < NP_REMOTE_HELLO_SIZE) {
		ssize_t n = recv(
			media->conn_fd, media->remote_hello + media->remote_hello_len,
			NP_REMOTE_HELLO_SIZE - media->remote_hello_len, MSG_DONTWAIT);
		if (n > 0) {
			media->remote_hello_len += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			pthread_mutex_unlock(&media->lock);
			return;
		}
		struct np_media_frame *discarded = close_connection_locked(media);
		pthread_mutex_unlock(&media->lock);
		free_frames(discarded);
		return;
	}
	if (!media->remote_handshake_ready) {
		const uint8_t *hello = media->remote_hello;
		if (memcmp(hello, "NPRH", 4) != 0 || hello[4] != 1 ||
		    hello[5] != NP_REMOTE_MEDIA_LANE || hello[6] != 0 || hello[7] != 0) {
			fprintf(stderr, "[media] invalid remote media handshake\n");
			struct np_media_frame *discarded = close_connection_locked(media);
			pthread_mutex_unlock(&media->lock);
			free_frames(discarded);
			return;
		}
		uint64_t token = 0;
		for (unsigned int index = 0; index < 8; index++)
			token |= (uint64_t)hello[8 + index] << (index * 8);
		if (token == 0) {
			struct np_media_frame *discarded = close_connection_locked(media);
			pthread_mutex_unlock(&media->lock);
			free_frames(discarded);
			return;
		}
		media->remote_session_token = token;
		media->remote_handshake_ready = true;
		media->just_attached = true;
	}
	// Media is write-only from this side. A readable condition means the peer
	// closed (FIN) or sent unexpected bytes — either way drop and re-accept.
	uint8_t scratch[64];
	for (;;) {
		ssize_t n = recv(media->conn_fd, scratch, sizeof(scratch), MSG_DONTWAIT);
		if (n > 0) {
			struct np_media_frame *discarded = close_connection_locked(media);
			fprintf(stderr, "[media] client sent unexpected data; detached\n");
			pthread_mutex_unlock(&media->lock);
			free_frames(discarded);
			return;
		}
		if (n == 0) {
			struct np_media_frame *discarded = close_connection_locked(media);
			fprintf(stderr, "[media] client detached\n");
			pthread_mutex_unlock(&media->lock);
			free_frames(discarded);
			return;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			pthread_mutex_unlock(&media->lock);
			return;
		}
		struct np_media_frame *discarded = close_connection_locked(media);
		fprintf(stderr, "[media] client detached: %s\n", strerror(errno));
		pthread_mutex_unlock(&media->lock);
		free_frames(discarded);
		return;
	}
}

bool np_media_connected(struct np_media *media) {
	if (!media || !media->lock_initialized) return false;
	pthread_mutex_lock(&media->lock);
	bool connected = media->conn_fd >= 0 && media->remote_handshake_ready;
	pthread_mutex_unlock(&media->lock);
	return connected;
}

uint64_t np_media_session_token(struct np_media *media) {
	if (!media || !media->lock_initialized) return 0;
	pthread_mutex_lock(&media->lock);
	uint64_t token = media->remote_handshake_ready
		? media->remote_session_token : 0;
	pthread_mutex_unlock(&media->lock);
	return token;
}

void np_media_set_session_ready(struct np_media *media, bool ready) {
	if (!media || !media->lock_initialized) return;
	pthread_mutex_lock(&media->lock);
	media->session_ready = ready && media->conn_fd >= 0 &&
	                       media->remote_handshake_ready;
	pthread_mutex_unlock(&media->lock);
}

void np_media_connection_identity(struct np_media *media, int *fd,
	                          uint64_t *generation) {
	if (fd) *fd = -1;
	if (generation) *generation = 0;
	if (!media || !media->lock_initialized) return;
	pthread_mutex_lock(&media->lock);
	if (fd) *fd = media->conn_fd;
	if (generation) *generation = media->connection_generation;
	pthread_mutex_unlock(&media->lock);
}

void np_media_disconnect(struct np_media *media) {
	if (!media || !media->lock_initialized) return;
	pthread_mutex_lock(&media->lock);
	struct np_media_frame *discarded = close_connection_locked(media);
	pthread_mutex_unlock(&media->lock);
	free_frames(discarded);
}

bool np_media_take_just_attached(struct np_media *media) {
	if (!media || !media->lock_initialized) return false;
	pthread_mutex_lock(&media->lock);
	bool attached = media->just_attached;
	media->just_attached = false;
	pthread_mutex_unlock(&media->lock);
	return attached;
}

bool np_media_send(struct np_media *media, uint8_t codec, uint8_t flags,
                   uint32_t surface_id,
                   uint32_t resource_id,
                   uint16_t width, uint16_t height, uint64_t pts_ns,
	                   uint16_t epoch, const uint8_t *payload, uint32_t length) {
	if (!media || !media->lock_initialized || !payload || !length) return false;
	size_t frame_size = NP_MEDIA_HEADER_SIZE + (size_t)length;

	pthread_mutex_lock(&media->lock);
	if (media->conn_fd < 0 || !media->remote_handshake_ready ||
	    !media->session_ready || media->writer_stopping) {
		pthread_mutex_unlock(&media->lock);
		return false;
	}
	uint64_t generation = media->connection_generation;
	size_t used = media->writer_queued_bytes +
	              media->writer_inflight_bytes + media->writer_reserved_bytes;
	if (length > NP_MEDIA_MAX_PAYLOAD || used > NP_MEDIA_WRITER_LIMIT ||
	    frame_size > NP_MEDIA_WRITER_LIMIT - used) {
		struct np_media_frame *discarded = close_connection_locked(media);
		pthread_mutex_unlock(&media->lock);
		free_frames(discarded);
		fprintf(stderr, "[media] writer queue limit exceeded; detached\n");
		return false;
	}
	/* Reserve before dropping the lock so concurrent encoder callbacks cannot
	 * all allocate a maximum-sized frame beyond the advertised queue cap. */
	media->writer_reserved_bytes += frame_size;
	pthread_mutex_unlock(&media->lock);

	struct np_media_frame *frame = malloc(sizeof(*frame) + frame_size);
	if (!frame) {
		pthread_mutex_lock(&media->lock);
		media->writer_reserved_bytes -= frame_size;
		struct np_media_frame *discarded = NULL;
		if (media->conn_fd >= 0 &&
		    media->connection_generation == generation)
			discarded = close_connection_locked(media);
		pthread_mutex_unlock(&media->lock);
		free_frames(discarded);
		return false;
	}
	frame->next = NULL;
	frame->generation = generation;
	frame->size = frame_size;

	uint8_t *hdr = frame->bytes;
	memset(hdr, 0, NP_MEDIA_HEADER_SIZE);
	memcpy(hdr, NP_MEDIA_MAGIC, 4);
	hdr[4] = NP_MEDIA_VERSION;
	hdr[5] = codec;
	hdr[6] = flags;
	hdr[7] = 0;
	memcpy(hdr + 8, &surface_id, 4);
	memcpy(hdr + 12, &resource_id, 4);
	memcpy(hdr + 16, &width, 2);
	memcpy(hdr + 18, &height, 2);
	memcpy(hdr + 20, &pts_ns, 8);
	memcpy(hdr + 28, &length, 4);
	memcpy(hdr + 32, &epoch, 2);
	memcpy(hdr + NP_MEDIA_HEADER_SIZE, payload, length);

	pthread_mutex_lock(&media->lock);
	media->writer_reserved_bytes -= frame_size;
	if (media->conn_fd < 0 || !media->remote_handshake_ready ||
	    !media->session_ready || media->writer_stopping ||
	    media->connection_generation != generation) {
		pthread_mutex_unlock(&media->lock);
		free(frame);
		return false;
	}
	if (media->writer_tail)
		media->writer_tail->next = frame;
	else
		media->writer_head = frame;
	media->writer_tail = frame;
	media->writer_queued_bytes += frame_size;
	pthread_cond_signal(&media->writer_cond);
	pthread_mutex_unlock(&media->lock);
	return true;
}
