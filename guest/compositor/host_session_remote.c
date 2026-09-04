// Host transport lanes, reconnect replay and session readiness.

#define _GNU_SOURCE

#include "compositor_internal.h"
#include "data_device.h"
#include "host_session_common.h"
#include "scene.h"
#include "window_events.h"
#include "windowwire.h"
#include "xwayland.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *session_environment_name(void)
{
	return "nativepipe-wayland.env";
}

static bool session_environment_path(char *path, size_t size)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !runtime[0]) return false;
	int written = snprintf(path, size, "%s/%s", runtime,
	                       session_environment_name());
	return written > 0 && (size_t)written < size;
}

static bool publish_session_environment(struct np_server *server)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *socket = server->session_socket;
	if (!runtime || !runtime[0] || !socket[0]) return false;

	char path[1024];
	char temporary[1088];
	if (!session_environment_path(path, sizeof(path))) return false;
	snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid());

	int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) {
		unlink(temporary);
		fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	}
	if (fd < 0) return false;

	FILE *env = fdopen(fd, "w");
	if (!env) {
		close(fd);
		unlink(temporary);
		return false;
	}
	bool ok = fprintf(env, "WAYLAND_DISPLAY=%s\nXDG_RUNTIME_DIR=%s\n",
	                  socket, runtime) > 0;
	const char *bus = getenv("DBUS_SESSION_BUS_ADDRESS");
	if (ok && bus && bus[0])
		ok = fprintf(env, "DBUS_SESSION_BUS_ADDRESS=%s\n", bus) > 0;
	if (ok && server->xwayland_display[0])
		ok = fprintf(env, "DISPLAY=%s\n", server->xwayland_display) > 0;
	if (ok && server->xwayland_auth[0])
		ok = fprintf(env, "XAUTHORITY=%s\n", server->xwayland_auth) > 0;
	if (ok) ok = fflush(env) == 0;
	if (ok) ok = fsync(fd) == 0;
	if (fclose(env) != 0) ok = false;
	if (ok) ok = rename(temporary, path) == 0;
	if (!ok) unlink(temporary);
	return ok;
}

static void unpublish_session_environment(void)
{
	char path[1024];
	if (!session_environment_path(path, sizeof(path))) return;
	if (unlink(path) < 0 && errno != ENOENT)
		fprintf(stderr, "[wayland] could not remove session readiness: %s\n",
		        strerror(errno));
}

static int host_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&server->host);
	np_host_pump(&server->host, np_input_handle_host_binary, server);
	np_presentation_flush(server);
	np_host_session_sync(server);
	wl_display_flush_clients(server->display);
	return 0;
}


/// Re-announces every live surface to a host that has just attached.
///
/// The host keeps no window state across a disconnect, and a client that is
/// merely idle will not commit again to prompt one. Without this the redial
/// succeeds and the windows still never come back, which makes the reconnect
/// path look like recovery without being it.
static void republish_state(struct np_server *server) {
	struct np_surface *surface;
	np_host_session_replay_metadata(server);
	if (!np_host_connected(&server->host)) return;

	/* A new host has no decoder history or media resources. Re-encode every
	 * current surface as an IDR and rebuild the same window scene graph used by
	 * the VM transport. Sending stale resource ids would leave the host waiting
	 * forever for frames that belonged to the previous TCP connection. */
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		(void)np_presentation_republish_remote(surface);
		if (!np_host_connected(&server->host)) break;
	}
}

static void discard_disconnected_host_reads(struct np_server *server) {
	np_data_host_disconnected(server);
}

static void close_host_session(struct np_server *server);

static bool all_host_channels_connected(struct np_server *server)
{
	if (!np_host_connected(&server->host)) return false;
	if (!np_media_connected(&server->media)) return false;
	uint64_t surface_token = np_host_session_token(&server->host);
	uint64_t media_token = np_media_session_token(&server->media);
	enum np_remote_pair_state pair = np_remote_pair_tokens(
		surface_token, media_token);
	if (pair == NP_REMOTE_PAIR_MISMATCHED) {
		fprintf(stderr, "[wayland] remote channel session tokens do not match\n");
		/* Surface is the admission lane. Keep its candidate token and reject only
		 * the mismatched media follower; a queued matching media lane can then
		 * attach without two simultaneous clients knocking each other out. */
		np_media_disconnect(&server->media);
		return false;
	}
	return pair == NP_REMOTE_PAIR_MATCHED;
}

static void close_host_session(struct np_server *server)
{
	np_host_set_output_enabled(&server->host, false);
	np_host_set_input_enabled(&server->host, false);
	np_media_set_session_ready(&server->media, false);
	np_host_disconnect(&server->host);
	np_media_disconnect(&server->media);
}

static void handle_media_attachment(struct np_server *server)
{
	if (!np_media_take_just_attached(&server->media)) return;
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->encoder) np_encoder_force_keyframe(surface->encoder);
	}
	fprintf(stderr, "[media] requested IDR on all encoders\n");
}

static int media_connection_readable(int fd, uint32_t mask, void *data)
{
	(void)fd;
	(void)mask;
	struct np_server *server = data;
	np_media_pump(&server->media);
	handle_media_attachment(server);
	np_host_session_sync(server);
	return 0;
}

static void sync_media_source(struct np_server *server)
{
	int fd;
	uint64_t generation;
	np_media_connection_identity(&server->media, &fd, &generation);
	if (server->watched_media_fd == fd &&
	    server->watched_media_generation == generation) return;
	if (server->media_connection_source) {
		wl_event_source_remove(server->media_connection_source);
		server->media_connection_source = NULL;
	}
	server->watched_media_fd = fd;
	server->watched_media_generation = generation;
	if (fd >= 0) {
		struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
		server->media_connection_source = wl_event_loop_add_fd(
			loop, fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
			media_connection_readable, server);
	}
}

void np_host_session_sync(struct np_server *server) {
	bool connected = all_host_channels_connected(server);
	if (server->host_session_ready && !connected) {
		server->host_session_ready = false;
		discard_disconnected_host_reads(server);
		/* Do not pair a newly dialled lane with sockets from the old generation. */
		close_host_session(server);
		connected = false;
	}

	if (!server->host_session_ready && connected) {
		/* channelReady is the host launch gate. The environment must be visible
		 * before the event because the host may launch immediately on receipt. */
		if (!publish_session_environment(server))
			fprintf(stderr, "[wayland] could not publish the session environment\n");
		uint32_t ready[] = {
			(uint32_t)getpid(), NP_WINDOW_PROTOCOL_VERSION,
		};
		/* Open the surface gate only for channelReady.  Everything emitted while
		 * the two TCP lanes were unpaired was deliberately discarded and will be
		 * reconstructed by republish_state below. */
		np_host_set_output_enabled(&server->host, true);
		server->host_session_ready = true;
		if (!np_window_event_send(server, NP_GUEST_SESSION_STARTED, ready, 2)) {
			server->host_session_ready = false;
			close_host_session(server);
			return;
		}
		np_host_set_input_enabled(&server->host, true);
		np_media_set_session_ready(&server->media, true);
		republish_state(server);
		/* Replay is ordinary ordered output and may itself discover a closed or
		 * backlogged surface lane. Fail both lanes in this same synchronization
		 * turn instead of leaving media enabled for a session that never received
		 * its authoritative snapshot. */
		if (!all_host_channels_connected(server)) {
			server->host_session_ready = false;
			discard_disconnected_host_reads(server);
			close_host_session(server);
		}
	}

	np_host_session_watch(
		server, &server->host, &server->host_connection_source,
		&server->watched_host_fd, &server->watched_host_mask,
		host_channel_readable);
	sync_media_source(server);
}

static int host_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&server->host, np_input_handle_host_binary, server);
	np_presentation_flush(server);
	np_host_session_sync(server);
	wl_display_flush_clients(server->display);
	return 0;
}


static int media_listener_readable(int fd, uint32_t mask, void *data) {
	(void)fd;
	(void)mask;
	struct np_server *server = data;
	np_media_accept(&server->media);
	handle_media_attachment(server);
	np_host_session_sync(server);
	return 0;
}

static int remote_pair_timeout(void *data)
{
	struct np_server *server = data;
	if (!server->host_session_ready &&
	    np_host_unpaired_expired(&server->host, 10000)) {
		fprintf(stderr, "[wayland] remote lane pairing timed out\n");
		close_host_session(server);
		discard_disconnected_host_reads(server);
		np_host_session_sync(server);
	}
	if (server->remote_pair_timeout_source)
		wl_event_source_timer_update(server->remote_pair_timeout_source, 1000);
	return 0;
}
void np_host_session_reset_readiness(void)
{
	unpublish_session_environment();
}

bool np_host_session_listen(struct np_server *server)
{
	return np_host_listen(&server->host, NP_SURFACE_PORT) &&
	       np_media_listen(&server->media);
}

bool np_host_session_set_socket(struct np_server *server, const char *socket)
{
	if (!socket) return false;
	if (strlen(socket) >= sizeof(server->session_socket)) return false;
	strcpy(server->session_socket, socket);
	/* SSH launches applications before either transport channel attaches. */
	if (!publish_session_environment(server)) return false;
	return true;
}

void np_host_session_attach(struct np_server *server, struct wl_event_loop *loop)
{
	wl_event_loop_add_fd(loop, server->host.listen_fd, WL_EVENT_READABLE,
	                     host_listener_readable, server);
	if (server->media.listen_fd >= 0)
		wl_event_loop_add_fd(loop, server->media.listen_fd, WL_EVENT_READABLE,
		                     media_listener_readable, server);
	server->remote_pair_timeout_source = wl_event_loop_add_timer(
		loop, remote_pair_timeout, server);
	if (server->remote_pair_timeout_source)
		wl_event_source_timer_update(server->remote_pair_timeout_source, 1000);
}

void np_host_session_finish(struct np_server *server)
{
	if (server->remote_pair_timeout_source) {
		wl_event_source_remove(server->remote_pair_timeout_source);
		server->remote_pair_timeout_source = NULL;
	}
	np_media_finish(&server->media);
	np_host_finish(&server->host);
	unpublish_session_environment();
}
