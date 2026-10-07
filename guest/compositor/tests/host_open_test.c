#define _GNU_SOURCE
#include "compositor_internal.h"
#include "host_open.h"
#include "windowwire.h"
#include "../../session/np-open-wire.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static unsigned requests, cancellations;
static uint32_t latest_token;
static unsigned char latest_frame[NP_OPEN_HEADER_SIZE + NP_OPEN_MAX_PAYLOAD];
static size_t latest_length;

bool np_backend_send_binary(struct np_server *server, const void *payload, size_t length)
{
	(void)server;
	struct np_window_reader reader;
	assert(np_window_reader_init(&reader, payload, length, NP_WINDOW_GUEST_TO_HOST));
	uint32_t token = np_window_read_u32(&reader);
	assert(token != 0);
	if (reader.opcode == NP_GUEST_HOST_OPEN_REQUESTED) {
		const unsigned char *bytes = NULL; size_t size = 0;
		assert(np_window_read_bytes(&reader, &bytes, &size, false, NULL));
		assert(size <= sizeof(latest_frame));
		memcpy(latest_frame, bytes, size); latest_length = size;
		requests++; latest_token = token;
	} else {
		assert(reader.opcode == NP_GUEST_HOST_OPEN_CANCELLED);
		cancellations++;
	}
	assert(np_window_reader_finished(&reader));
	return true;
}

static int peer(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	assert(fd >= 0);
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	strcpy(address.sun_path, path);
	assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	return fd;
}

static void pump(struct np_server *server, unsigned turns)
{
	for (unsigned i = 0; i < turns; i++)
		assert(wl_event_loop_dispatch(wl_display_get_event_loop(server->display), 2) == 0);
}

static void send_request(int fd, const char *value)
{
	unsigned char frame[NP_OPEN_HEADER_SIZE + NP_OPEN_MAX_PAYLOAD];
	size_t size = np_open_encode_request(NP_OPEN_KIND_URL, value, frame, sizeof(frame));
	assert(size && send(fd, frame, size, MSG_NOSIGNAL) == (ssize_t)size);
}

static void optional_response(struct np_server *server, uint32_t token,
                              const unsigned char *frame, size_t length, int malformed)
{
	struct np_window_message message;
	np_window_message_init(&message, NP_WINDOW_HOST_TO_GUEST, NP_HOST_OPEN_RESPONSE);
	if (malformed != 1) {
		np_window_put_u32(&message, malformed == 4 ? 0 : token);
		if (malformed != 2) np_window_put_bytes(&message, frame, length);
	}
	if (malformed == 5) np_window_put_u8(&message, 0);
	if (malformed == 6) message.len--;
	struct np_window_reader reader;
	assert(np_window_reader_init(&reader, message.data, message.len, NP_WINDOW_HOST_TO_GUEST));
	assert(np_host_open_handle_command(server, &reader));
	/* The outer wire header remains strict, including for optional messages. */
	message.data[6] = 1;
	assert(!np_window_reader_init(&reader, message.data, message.len, NP_WINDOW_HOST_TO_GUEST));
	np_window_message_clear(&message);
}

int main(void)
{
	struct np_server server = {0};
	server.display = wl_display_create(); server.host_session_ready = true;
	assert(server.display && np_host_open_init(&server));
	char path[108]; strcpy(path, getenv("NATIVEPIPE_OPEN_SOCKET"));
	struct stat metadata;
	assert(stat(path, &metadata) == 0 && (metadata.st_mode & 0777) == 0600);
	char directory[108]; strcpy(directory, path); *strrchr(directory, '/') = 0;
	assert(stat(directory, &metadata) == 0 && (metadata.st_mode & 0777) == 0700);

	int client = peer(path);
	send_request(client, "https://example.com");
	pump(&server, 4);
	assert(requests == 1 && latest_token != 0);
	assert(latest_length == 31 && memcmp(latest_frame, "NPOP\1\1\0\0\23\0\0\0https://example.com", 31) == 0);
	uint32_t completed_token = latest_token;
	unsigned char success[NP_OPEN_HEADER_SIZE] = {'N', 'P', 'O', 'R', 1, 0, 0, 0, 0, 0, 0, 0};
	for (int malformed = 1; malformed <= 8; malformed++) {
		unsigned char bad[NP_OPEN_HEADER_SIZE + 1]; memcpy(bad, success, sizeof(success));
		if (malformed == 3) bad[5] = 99;
		if (malformed == 7) { np_open_put_u32(bad + 8, 1); bad[NP_OPEN_HEADER_SIZE] = 0xff; }
		optional_response(&server, malformed == 8 ? completed_token + 1 : completed_token,
		                  bad, sizeof(success) + (malformed == 7), malformed);
		pump(&server, 2);
		unsigned char extra;
		assert(recv(client, &extra, 1, 0) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
		assert(server.host_session_ready && cancellations == 0);
	}
	/* A later valid command still reaches the original caller. */
	optional_response(&server, completed_token, success, sizeof(success), 0);
	pump(&server, 4);
	unsigned char response[NP_OPEN_HEADER_SIZE + NP_OPEN_MAX_MESSAGE];
	assert(recv(client, response, sizeof(response), 0) == NP_OPEN_HEADER_SIZE);
	assert(!memcmp(response, success, sizeof(success)) && cancellations == 0);
	close(client);
	assert(np_host_open_response(&server, completed_token, success, sizeof(success))); /* Late responses are harmless. */

	client = peer(path); send_request(client, "https://example.com/cancel"); pump(&server, 4);
	uint32_t cancelled_token = latest_token;
	close(client); pump(&server, 4);
	assert(cancellations == 1);
	assert(np_host_open_response(&server, cancelled_token, success, sizeof(success)));
	pump(&server, 2);

	unsigned before = requests;
	client = peer(path);
	unsigned char invalid[] = {'N', 'P', 'O', 'P', 1, 1, 0, 0, 1, 0, 0, 0, 0xff};
	assert(send(client, invalid, sizeof(invalid), MSG_NOSIGNAL) == (ssize_t)sizeof(invalid));
	pump(&server, 4);
	ssize_t size = recv(client, response, sizeof(response), 0);
	assert(size >= NP_OPEN_HEADER_SIZE && response[5] == NP_OPEN_STATUS_FAILED && requests == before);
	close(client);
	invalid[5] = 9;
	client = peer(path);
	assert(send(client, invalid, NP_OPEN_HEADER_SIZE, MSG_NOSIGNAL) == NP_OPEN_HEADER_SIZE);
	pump(&server, 4);
	assert(recv(client, response, sizeof(response), 0) >= NP_OPEN_HEADER_SIZE && requests == before);
	close(client);

	assert(!np_host_open_response(&server, 0, success, sizeof(success)));
	success[5] = 4; assert(!np_host_open_response(&server, 1, success, sizeof(success))); success[5] = 0;
	assert(!np_host_open_response(&server, 1, success, sizeof(success) - 1));

	int clients[16];
	for (unsigned i = 0; i < 16; i++) { clients[i] = peer(path); send_request(clients[i], "https://example.com/queued"); }
	pump(&server, 5); assert(requests == before + 16);
	client = peer(path); pump(&server, 4);
	assert(recv(client, response, sizeof(response), 0) >= NP_OPEN_HEADER_SIZE && response[5] == NP_OPEN_STATUS_FAILED);
	close(client);
	for (unsigned i = 0; i < 16; i++) close(clients[i]);
	pump(&server, 4); assert(cancellations == 17);

	/* A partial writer must not hold one of the bounded slots forever. */
	client = peer(path); assert(send(client, "NP", 2, MSG_NOSIGNAL) == 2); pump(&server, 3);
	assert(wl_event_loop_dispatch(wl_display_get_event_loop(server.display), 10100) == 0);
	assert(recv(client, response, sizeof(response), 0) == 0); close(client);

	client = peer(path); send_request(client, "https://example.com/disconnect"); pump(&server, 4);
	np_host_open_finish(&server);
	assert(cancellations == 18 && access(path, F_OK) < 0 && access(directory, F_OK) < 0);
	assert(getenv("NATIVEPIPE_OPEN_SOCKET") == NULL);
	assert(recv(client, response, sizeof(response), 0) == 0); close(client);
	wl_display_destroy(server.display);
	puts("host-open: strict framing, ignored malformed optional replies, subsequent valid reply, privacy modes, peer cancellation, bounded requests, timeout and shutdown PASS");
	return 0;
}
