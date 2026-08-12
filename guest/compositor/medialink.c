#include "medialink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool np_media_listen(struct np_media *media) {
	memset(media, 0, sizeof(*media));
	media->listen_fd = -1;
	media->conn_fd = -1;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return false;
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
		return false;
	}
	set_nonblocking(fd);
	media->listen_fd = fd;
	fprintf(stderr, "[media] listening on 127.0.0.1:%d\n", NP_MEDIA_PORT);
	return true;
}

void np_media_finish(struct np_media *media) {
	if (media->conn_fd >= 0) close(media->conn_fd);
	if (media->listen_fd >= 0) close(media->listen_fd);
	memset(media, 0, sizeof(*media));
	media->listen_fd = -1;
	media->conn_fd = -1;
}

void np_media_accept(struct np_media *media) {
	if (media->listen_fd < 0 || media->conn_fd >= 0) return;
	int fd = accept(media->listen_fd, NULL, NULL);
	if (fd < 0) return;
	set_nonblocking(fd);
	media->conn_fd = fd;
	media->just_attached = true;
	fprintf(stderr, "[media] client attached\n");
}

void np_media_pump(struct np_media *media) {
	if (!media || media->conn_fd < 0) return;
	// Media is write-only from this side. A readable condition means the peer
	// closed (FIN) or sent unexpected bytes — either way drop and re-accept.
	uint8_t scratch[64];
	for (;;) {
		ssize_t n = recv(media->conn_fd, scratch, sizeof(scratch), MSG_DONTWAIT);
		if (n > 0) continue;
		if (n == 0) {
			close(media->conn_fd);
			media->conn_fd = -1;
			fprintf(stderr, "[media] client detached\n");
			return;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) return;
		close(media->conn_fd);
		media->conn_fd = -1;
		fprintf(stderr, "[media] client detached: %s\n", strerror(errno));
		return;
	}
}

bool np_media_connected(const struct np_media *media) {
	return media && media->conn_fd >= 0;
}

static bool write_all(int fd, const void *buf, size_t len) {
	const uint8_t *p = buf;
	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n > 0) {
			p += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			usleep(1000);
			continue;
		}
		return false;
	}
	return true;
}

bool np_media_send(struct np_media *media, uint32_t surface_id,
                   uint16_t width, uint16_t height, uint64_t pts_ns,
                   uint16_t epoch, const uint8_t *payload, uint32_t length) {
	if (!np_media_connected(media) || !payload || !length) return false;

	uint8_t hdr[NP_MEDIA_HEADER_SIZE];
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, NP_MEDIA_MAGIC, 4);
	hdr[4] = NP_MEDIA_VERSION;
	hdr[5] = NP_MEDIA_CODEC_H264;
	hdr[6] = 0;
	hdr[7] = 0;
	memcpy(hdr + 8, &surface_id, 4);
	memcpy(hdr + 12, &width, 2);
	memcpy(hdr + 14, &height, 2);
	memcpy(hdr + 16, &pts_ns, 8);
	memcpy(hdr + 24, &length, 4);
	memcpy(hdr + 28, &epoch, 2);

	if (!write_all(media->conn_fd, hdr, sizeof(hdr)) ||
	    !write_all(media->conn_fd, payload, length)) {
		close(media->conn_fd);
		media->conn_fd = -1;
		fprintf(stderr, "[media] client detached\n");
		return false;
	}
	return true;
}
