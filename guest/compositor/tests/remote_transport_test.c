#include "hostlink.h"
#include "medialink.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void assert_no_bytes(int fd)
{
	uint8_t byte;
	ssize_t count = recv(fd, &byte, sizeof(byte), MSG_DONTWAIT);
	assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
}

static void make_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	assert(flags >= 0);
	assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static void write_hello(int fd, uint8_t lane, uint64_t token)
{
	uint8_t hello[16] = { 'N', 'P', 'R', 'H', 1, lane, 0, 0 };
	for (unsigned int index = 0; index < 8; index++)
		hello[8 + index] = (uint8_t)(token >> (index * 8));
	assert(send(fd, hello, sizeof(hello), 0) == (ssize_t)sizeof(hello));
}

static int handled_commands;

static bool count_command(const unsigned char *payload, size_t length,
                          void *user_data)
{
	(void)user_data;
	assert(length == 4);
	assert(payload[0] == 0x44);
	handled_commands++;
	return true;
}

static void write_command(int fd)
{
	const uint8_t frame[16] = {
		'N', 'P', 'I', 'P', 1, 0, 0, 0, 4, 0, 0, 0,
		0x44, 0, 0, 0,
	};
	assert(send(fd, frame, sizeof(frame), 0) == (ssize_t)sizeof(frame));
}

static void lane_hellos_and_pairing_are_validated(void)
{
	const uint64_t token = UINT64_C(0x0102030405060708);
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	make_nonblocking(sockets[0]);
	struct np_host host;
	memset(&host, 0, sizeof(host));
	host.listen_fd = -1;
	host.conn_fd = sockets[0];
	uint8_t hello[16] = { 'N', 'P', 'R', 'H', 1, 1, 0, 0 };
	for (unsigned int index = 0; index < 8; index++)
		hello[8 + index] = (uint8_t)(token >> (index * 8));
	assert(send(sockets[1], hello, 5, 0) == 5);
	np_host_pump(&host, count_command, NULL);
	assert(!np_host_connected(&host));
	assert(send(sockets[1], hello + 5, sizeof(hello) - 5, 0) ==
	       (ssize_t)(sizeof(hello) - 5));
	np_host_pump(&host, count_command, NULL);
	assert(np_host_connected(&host));
	assert(np_host_session_token(&host) == token);
	np_host_finish(&host);
	close(sockets[1]);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	make_nonblocking(sockets[0]);
	memset(&host, 0, sizeof(host));
	host.listen_fd = -1;
	host.conn_fd = sockets[0];
	write_hello(sockets[1], 1, token);
	np_host_pump(&host, count_command, NULL);
	assert(np_host_connected(&host));
	write_command(sockets[1]);
	handled_commands = 0;
	np_host_pump(&host, count_command, NULL);
	assert(handled_commands == 0);
	assert(host.conn_fd < 0);
	np_host_finish(&host);
	close(sockets[1]);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	make_nonblocking(sockets[0]);
	memset(&host, 0, sizeof(host));
	host.listen_fd = -1;
	host.conn_fd = sockets[0];
	write_hello(sockets[1], 2, token);
	np_host_pump(&host, NULL, NULL);
	assert(host.conn_fd < 0);
	np_host_finish(&host);
	close(sockets[1]);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	make_nonblocking(sockets[0]);
	struct np_media media;
	assert(np_media_init(&media));
	media.conn_fd = sockets[0];
	write_hello(sockets[1], 2, token);
	np_media_pump(&media);
	assert(np_media_connected(&media));
	assert(np_media_session_token(&media) == token);
	uint8_t unexpected = 1;
	assert(send(sockets[1], &unexpected, 1, 0) == 1);
	np_media_pump(&media);
	assert(!np_media_connected(&media));
	np_media_finish(&media);
	close(sockets[1]);

	assert(np_remote_pair_tokens(0, token) == NP_REMOTE_PAIR_INCOMPLETE);
	assert(np_remote_pair_tokens(token, token) == NP_REMOTE_PAIR_MATCHED);
	assert(np_remote_pair_tokens(token, token + 1) == NP_REMOTE_PAIR_MISMATCHED);
}

static int local_listener(char path[sizeof(((struct sockaddr_un *)0)->sun_path)])
{
	char temporary[] = "/tmp/nativepipe-transport.XXXXXX";
	int placeholder = mkstemp(temporary);
	assert(placeholder >= 0);
	close(placeholder);
	assert(unlink(temporary) == 0);
	assert(strlen(temporary) < sizeof(((struct sockaddr_un *)0)->sun_path));
	strcpy(path, temporary);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(fd >= 0);
	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		int code = errno;
		close(fd);
		if (code == EPERM || code == EACCES) return -1;
		assert(!"local listener bind failed");
	}
	assert(listen(fd, 4) == 0);
	make_nonblocking(fd);
	return fd;
}

static int local_dial(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(fd >= 0);
	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);
	assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	return fd;
}

static void half_handshake_can_be_replaced_without_listener_spin(void)
{
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	struct np_host host;
	memset(&host, 0, sizeof(host));
	host.listen_fd = local_listener(path);
	if (host.listen_fd < 0) {
		fprintf(stderr, "remote_transport_test: local accept subtest skipped (sandbox)\n");
		return;
	}
	host.conn_fd = -1;

	int first_peer = local_dial(path);
	np_host_accept(&host);
	int first_host = host.conn_fd;
	assert(first_host >= 0);

	int replacement_peer = local_dial(path);
	np_host_accept(&host);
	assert(host.conn_fd >= 0 && host.conn_fd != first_host);
	uint8_t byte;
	assert(recv(first_peer, &byte, sizeof(byte), 0) == 0);

	/* Once paired, an unrelated lane is drained and closed without replacing
	 * the live descriptor. */
	host.remote_handshake_ready = true;
	np_host_set_output_enabled(&host, true);
	int live_host = host.conn_fd;
	int rejected_peer = local_dial(path);
	np_host_accept(&host);
	assert(host.conn_fd == live_host);
	assert(recv(rejected_peer, &byte, sizeof(byte), 0) == 0);

	close(first_peer);
	close(replacement_peer);
	close(rejected_peer);
	np_host_finish(&host);
	assert(unlink(path) == 0);
}

static void surface_output_waits_for_paired_session(void)
{
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	struct np_host host;
	memset(&host, 0, sizeof(host));
	host.listen_fd = -1;
	host.conn_fd = sockets[0];
	const uint8_t payload[] = { 0x0d, 0, 0, 0 };

	assert(!np_host_send_binary(&host, payload, sizeof(payload)));
	assert_no_bytes(sockets[1]);

	np_host_set_output_enabled(&host, true);
	assert(np_host_send_binary(&host, payload, sizeof(payload)));
	uint8_t framed[16];
	assert(recv(sockets[1], framed, sizeof(framed), MSG_WAITALL) ==
	       (ssize_t)sizeof(framed));
	assert(memcmp(framed, "NPIP", 4) == 0);
	assert(memcmp(framed + 12, payload, sizeof(payload)) == 0);

	np_host_finish(&host);
	close(sockets[1]);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	memset(&host, 0, sizeof(host));
	host.listen_fd = -1;
	host.conn_fd = sockets[0];
	np_host_set_output_enabled(&host, true);
	close(sockets[1]);
	assert(!np_host_send_binary(&host, payload, sizeof(payload)));
	assert(host.conn_fd < 0);
	np_host_finish(&host);
}

static void media_output_waits_for_channel_ready(void)
{
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	make_nonblocking(sockets[0]);
	struct np_media media;
	assert(np_media_init(&media));
	media.conn_fd = sockets[0];
	media.remote_handshake_ready = true;
	const uint8_t alpha[] = { 9, 8 };
	const uint8_t payload[] = { 1, 2, 3 };

	assert(!np_media_send(&media, 1, 0, 1, 2, 8, 8, 0, 0,
	                      payload, sizeof(payload)));
	assert_no_bytes(sockets[1]);

	np_media_set_session_ready(&media, true);
	assert(np_media_send(&media, NP_MEDIA_CODEC_ALPHA_RLE, 0,
	                     1, 2, 8, 8, 0, 0, alpha, sizeof(alpha)));
	assert(np_media_send(&media, 1, 0, 1, 2, 8, 8, 0, 0,
	                     payload, sizeof(payload)));
	uint8_t alpha_frame[NP_MEDIA_HEADER_SIZE + sizeof(alpha)];
	assert(recv(sockets[1], alpha_frame, sizeof(alpha_frame), MSG_WAITALL) ==
	       (ssize_t)sizeof(alpha_frame));
	assert(memcmp(alpha_frame, NP_MEDIA_MAGIC, 4) == 0);
	assert(alpha_frame[5] == NP_MEDIA_CODEC_ALPHA_RLE);
	assert(memcmp(alpha_frame + NP_MEDIA_HEADER_SIZE, alpha, sizeof(alpha)) == 0);
	uint8_t video_frame[NP_MEDIA_HEADER_SIZE + sizeof(payload)];
	assert(recv(sockets[1], video_frame, sizeof(video_frame), MSG_WAITALL) ==
	       (ssize_t)sizeof(video_frame));
	assert(memcmp(video_frame, NP_MEDIA_MAGIC, 4) == 0);
	assert(video_frame[5] == NP_MEDIA_CODEC_H264);
	assert(memcmp(video_frame + NP_MEDIA_HEADER_SIZE,
	              payload, sizeof(payload)) == 0);

	np_media_finish(&media);
	close(sockets[1]);
}

static double monotonic_seconds(void)
{
	struct timespec now;
	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void bounded_writer_does_not_hold_state_lock(void)
{
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	make_nonblocking(sockets[0]);
	int send_buffer = 4096;
	assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
	                  &send_buffer, sizeof(send_buffer)) == 0);

	struct np_media media;
	assert(np_media_init(&media));
	media.conn_fd = sockets[0];
	media.remote_handshake_ready = true;
	media.remote_session_token = UINT64_C(0x8899aabbccddeeff);
	np_media_set_session_ready(&media, true);

	const size_t maximum = 32u * 1024u * 1024u;
	uint8_t *payload = malloc(maximum);
	assert(payload);
	memset(payload, 0x5a, maximum);
	assert(np_media_send(&media, NP_MEDIA_CODEC_H264, 0,
	                     9, 10, 64, 64, 1, 1, payload, (uint32_t)maximum));

	bool inflight = false;
	for (unsigned int attempt = 0; attempt < 1000; attempt++) {
		pthread_mutex_lock(&media.lock);
		inflight = media.writer_inflight_bytes != 0;
		pthread_mutex_unlock(&media.lock);
		if (inflight) break;
		usleep(1000);
	}
	assert(inflight);

	double before = monotonic_seconds();
	assert(np_media_connected(&media));
	assert(np_media_session_token(&media) == UINT64_C(0x8899aabbccddeeff));
	assert(monotonic_seconds() - before < 0.25);

	/* One maximum frame is in flight and one is queued. A third frame must
	 * fail the current lane closed instead of growing memory without bound. */
	assert(np_media_send(&media, NP_MEDIA_CODEC_H264, 0,
	                     9, 11, 64, 64, 2, 1, payload, (uint32_t)maximum));
	const uint8_t overflow = 1;
	assert(!np_media_send(&media, NP_MEDIA_CODEC_H264, 0,
	                      9, 12, 64, 64, 3, 1, &overflow, sizeof(overflow)));
	assert(!np_media_connected(&media));

	/* The old writer still owns a duplicate of the failed socket. Reuse the
	 * media object's connection slot immediately and prove its generation check
	 * cannot close or write through the replacement descriptor. */
	int replacement[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0);
	make_nonblocking(replacement[0]);
	pthread_mutex_lock(&media.lock);
	media.connection_generation++;
	media.conn_fd = replacement[0];
	media.remote_handshake_ready = true;
	media.remote_session_token = UINT64_C(0x1020304050607080);
	media.session_ready = true;
	pthread_mutex_unlock(&media.lock);
	assert(np_media_send(&media, NP_MEDIA_CODEC_H264, 0,
	                     9, 13, 64, 64, 4, 2, &overflow, sizeof(overflow)));
	uint8_t replacement_frame[NP_MEDIA_HEADER_SIZE + sizeof(overflow)];
	assert(recv(replacement[1], replacement_frame,
	            sizeof(replacement_frame), MSG_WAITALL) ==
	       (ssize_t)sizeof(replacement_frame));
	assert(replacement_frame[5] == NP_MEDIA_CODEC_H264);
	assert(np_media_connected(&media));

	before = monotonic_seconds();
	np_media_finish(&media);
	assert(monotonic_seconds() - before < 2.0);
	free(payload);
	close(sockets[1]);
	close(replacement[1]);
}

int main(void)
{
	lane_hellos_and_pairing_are_validated();
	half_handshake_can_be_replaced_without_listener_spin();
	surface_output_waits_for_paired_session();
	media_output_waits_for_channel_ready();
	bounded_writer_does_not_hold_state_lock();
	return 0;
}
