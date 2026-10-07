#define _GNU_SOURCE

#include "host_open.h"
#include "compositor_internal.h"
#include "window_events.h"
#include "../session/np-open-wire.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define NP_OPEN_MAX_PENDING 16
#define NP_OPEN_IO_TIMEOUT_MS 10000

struct np_host_open;
struct np_host_open_client {
	struct wl_list link;
	struct np_host_open *owner;
	struct wl_event_source *source, *timer;
	int fd;
	uint32_t token;
	unsigned char input[NP_OPEN_HEADER_SIZE + NP_OPEN_MAX_PAYLOAD];
	size_t received, expected;
	unsigned char output[NP_OPEN_HEADER_SIZE + NP_OPEN_MAX_MESSAGE];
	size_t sent, output_length;
};

struct np_host_open {
	struct np_server *server;
	struct wl_list clients;
	struct wl_event_source *source;
	int fd;
	unsigned count;
	uint32_t next_token;
	char directory[64], path[108];
};

static void close_client(struct np_host_open_client *client)
{
	if (client->token) {
		uint32_t token = client->token;
		(void)np_window_event_send(client->owner->server,
		                           NP_GUEST_HOST_OPEN_CANCELLED, &token, 1);
	}
	if (client->source) wl_event_source_remove(client->source);
	if (client->timer) wl_event_source_remove(client->timer);
	close(client->fd);
	wl_list_remove(&client->link);
	client->owner->count--;
	free(client);
}

static int io_expired(void *data)
{
	close_client(data);
	return 0;
}

static size_t failure_frame(unsigned char *frame, const char *message)
{
	size_t length = strlen(message);
	if (length > NP_OPEN_MAX_MESSAGE) length = NP_OPEN_MAX_MESSAGE;
	memcpy(frame, NP_OPEN_MAGIC_RESPONSE, 4);
	frame[4] = NP_OPEN_VERSION;
	frame[5] = NP_OPEN_STATUS_FAILED;
	frame[6] = frame[7] = 0;
	np_open_put_u32(frame + 8, (uint32_t)length);
	memcpy(frame + NP_OPEN_HEADER_SIZE, message, length);
	return NP_OPEN_HEADER_SIZE + length;
}

static void respond_failure(struct np_host_open_client *client, const char *message)
{
	client->output_length = failure_frame(client->output, message);
	client->sent = 0;
	wl_event_source_fd_update(client->source, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
	wl_event_source_timer_update(client->timer, NP_OPEN_IO_TIMEOUT_MS);
}

static bool request_header_valid(struct np_host_open_client *client)
{
	const unsigned char *frame = client->input;
	uint32_t length = np_open_get_u32(frame + 8);
	if (memcmp(frame, NP_OPEN_MAGIC_REQUEST, 4) || frame[4] != NP_OPEN_VERSION ||
	    (frame[5] != NP_OPEN_KIND_URL && frame[5] != NP_OPEN_KIND_FILE) ||
	    frame[6] || frame[7] || !length || length > NP_OPEN_MAX_PAYLOAD)
		return false;
	client->expected = NP_OPEN_HEADER_SIZE + length;
	return true;
}

static bool forward_request(struct np_host_open_client *client)
{
	const unsigned char *payload = client->input + NP_OPEN_HEADER_SIZE;
	size_t length = client->expected - NP_OPEN_HEADER_SIZE;
	for (size_t i = 0; i < length; i++)
		if (payload[i] < 0x20 || payload[i] == 0x7f) return false;
	if (!g_utf8_validate((const char *)payload, (gssize)length, NULL)) return false;
	struct np_host_open *owner = client->owner;
	if (!owner->server->host_session_ready) return false;
	/* A caller can leave an approval open while many other calls finish. Even
	 * after counter wrap, never reuse one of those still-live tokens. */
	bool used;
	do {
		uint32_t token = owner->next_token++;
		used = token == 0;
		struct np_host_open_client *pending;
		wl_list_for_each(pending, &owner->clients, link)
			if (pending->token == token) used = true;
		if (!used) client->token = token;
	} while (used);
	struct np_window_message message;
	np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST, NP_GUEST_HOST_OPEN_REQUESTED);
	np_window_put_u32(&message, client->token);
	np_window_put_bytes(&message, client->input, client->expected);
	bool sent = np_window_event_send_message(owner->server, &message);
	np_window_message_clear(&message);
	if (!sent) { client->token = 0; return false; }
	/* Approval and file transfer have no total deadline. A user may take time to
	 * decide, and a large file may keep transferring. EOF remains observable. */
	wl_event_source_timer_update(client->timer, 0);
	return true;
}

static int client_ready(int fd, uint32_t mask, void *data)
{
	struct np_host_open_client *client = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) { close_client(client); return 0; }
	if (mask & WL_EVENT_READABLE) {
		for (;;) {
			unsigned char extra;
			bool waiting = client->received == client->expected;
			void *destination = waiting ? &extra : client->input + client->received;
			size_t available = waiting ? 1 : client->expected - client->received;
			ssize_t count = recv(fd, destination, available, 0);
			if (count < 0 && errno == EINTR) continue;
			if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
			if (count <= 0 || waiting) { close_client(client); return 0; }
			client->received += (size_t)count;
			if (client->received == NP_OPEN_HEADER_SIZE && client->expected == NP_OPEN_HEADER_SIZE) {
				if (!request_header_valid(client)) {
					respond_failure(client, "Invalid host open request.");
					break;
				}
			}
			if (client->received == client->expected) {
				if (!forward_request(client)) respond_failure(client, "The Mac cannot receive this open request.");
				break;
			}
		}
	}
	if ((mask & WL_EVENT_WRITABLE) && client->output_length) {
		while (client->sent < client->output_length) {
			ssize_t count = send(fd, client->output + client->sent,
			                     client->output_length - client->sent, MSG_NOSIGNAL);
			if (count < 0 && errno == EINTR) continue;
			if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
			if (count <= 0) { close_client(client); return 0; }
			client->sent += (size_t)count;
			wl_event_source_timer_update(client->timer, NP_OPEN_IO_TIMEOUT_MS);
		}
		close_client(client);
	}
	return 0;
}

static int accept_ready(int fd, uint32_t mask, void *data)
{
	(void)mask;
	struct np_host_open *owner = data;
	/* Bound each drain too: a local flood must not starve window/input events. */
	for (unsigned i = 0; i < NP_OPEN_MAX_PENDING; i++) {
		int peer = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (peer < 0 && errno == EINTR) { i--; continue; }
		if (peer < 0) break;
		struct ucred credentials;
		socklen_t size = sizeof(credentials);
		if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0 ||
		    credentials.uid != getuid()) { close(peer); continue; }
		if (owner->count >= NP_OPEN_MAX_PENDING) {
			unsigned char frame[NP_OPEN_HEADER_SIZE + NP_OPEN_MAX_MESSAGE];
			size_t length = failure_frame(frame, "Too many pending host open requests.");
			(void)send(peer, frame, length, MSG_NOSIGNAL);
			close(peer);
			continue;
		}
		struct np_host_open_client *client = calloc(1, sizeof(*client));
		if (!client) { close(peer); continue; }
		client->owner = owner; client->fd = peer; client->expected = NP_OPEN_HEADER_SIZE;
		wl_list_insert(owner->clients.prev, &client->link);
		owner->count++;
		struct wl_event_loop *loop = wl_display_get_event_loop(owner->server->display);
		client->source = wl_event_loop_add_fd(loop, peer, WL_EVENT_READABLE, client_ready, client);
		client->timer = wl_event_loop_add_timer(loop, io_expired, client);
		if (!client->source || !client->timer) { close_client(client); continue; }
		wl_event_source_timer_update(client->timer, NP_OPEN_IO_TIMEOUT_MS);
	}
	return 0;
}

bool np_host_open_init(struct np_server *server)
{
	if (!server || server->host_open) return false;
	struct np_host_open *owner = calloc(1, sizeof(*owner));
	if (!owner) return false;
	owner->server = server; owner->fd = -1; owner->next_token = 1;
	wl_list_init(&owner->clients);
	strcpy(owner->directory, "/tmp/nativepipe-open-XXXXXXXX");
	if (!mkdtemp(owner->directory)) { owner->directory[0] = 0; goto fail; }
	if (chmod(owner->directory, 0700) < 0) goto fail;
	snprintf(owner->path, sizeof(owner->path), "%s/open", owner->directory);
	owner->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (owner->fd < 0) goto fail;
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	strcpy(address.sun_path, owner->path);
	if (bind(owner->fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    chmod(owner->path, 0600) < 0 || listen(owner->fd, NP_OPEN_MAX_PENDING) < 0) goto fail;
	owner->source = wl_event_loop_add_fd(wl_display_get_event_loop(server->display),
	                                    owner->fd, WL_EVENT_READABLE, accept_ready, owner);
	if (!owner->source || setenv("NATIVEPIPE_OPEN_SOCKET", owner->path, 1) < 0) goto fail;
	server->host_open = owner;
	return true;
fail:
	if (owner->source) wl_event_source_remove(owner->source);
	if (owner->fd >= 0) close(owner->fd);
	if (owner->path[0]) unlink(owner->path);
	if (owner->directory[0]) rmdir(owner->directory);
	free(owner);
	return false;
}

bool np_host_open_response(struct np_server *server, uint32_t token,
	                       const unsigned char *frame, size_t length)
{
	uint8_t status; uint32_t payload_length;
	if (!token || !frame || length < NP_OPEN_HEADER_SIZE ||
	    np_open_parse_response_header(frame, &status, &payload_length) < 0 ||
	    length != NP_OPEN_HEADER_SIZE + payload_length ||
	    !g_utf8_validate((const char *)frame + NP_OPEN_HEADER_SIZE, payload_length, NULL)) return false;
	struct np_host_open *owner = server ? server->host_open : NULL;
	if (!owner) return true; /* The corresponding client/session already ended. */
	struct np_host_open_client *client;
	wl_list_for_each(client, &owner->clients, link) {
		if (client->token != token) continue;
		client->token = 0;
		memcpy(client->output, frame, length); client->output_length = length; client->sent = 0;
		wl_event_source_fd_update(client->source, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
		wl_event_source_timer_update(client->timer, NP_OPEN_IO_TIMEOUT_MS);
		break;
	}
	return true; /* A cancelled token's late response is harmless. */
}

bool np_host_open_handle_command(struct np_server *server, struct np_window_reader *reader)
{
	uint32_t token = np_window_read_u32(reader);
	size_t size = 0;
	const unsigned char *frame = NULL;
	if (np_window_read_bytes(reader, &frame, &size, false, NULL) &&
	    np_window_reader_finished(reader))
		(void)np_host_open_response(server, token, frame, size);
	return true; /* Ignore an invalid optional reply, not the graphical session. */
}

void np_host_open_finish(struct np_server *server)
{
	struct np_host_open *owner = server ? server->host_open : NULL;
	if (!owner) return;
	server->host_open = NULL;
	struct np_host_open_client *client, *next;
	wl_list_for_each_safe(client, next, &owner->clients, link) close_client(client);
	if (owner->source) wl_event_source_remove(owner->source);
	close(owner->fd);
	unlink(owner->path); rmdir(owner->directory);
	const char *exported = getenv("NATIVEPIPE_OPEN_SOCKET");
	if (exported && !strcmp(exported, owner->path)) unsetenv("NATIVEPIPE_OPEN_SOCKET");
	free(owner);
}
