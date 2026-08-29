// Host transport lanes, reconnect replay and session readiness.

#define _GNU_SOURCE

#include "compositor_internal.h"
#include "scene.h"
#include "window_events.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *session_environment_name(void)
{
	return "remotepipe-wayland.env";
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
	np_host_pump(&server->host, np_input_handle_host_command, np_input_handle_host_binary, server);
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

	// Surfaces first, and in creation order: a role or a parent reference is
	// meaningless to the host until the surface it names exists.
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		uint32_t fields[] = {surface->id};
		np_window_event_send(server, NP_GUEST_SURFACE_CREATED, fields, 1);
	}

	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		cJSON *body;
		if (np_surface_is_toplevel(surface)) {
			uint32_t fields[] = {surface->window_id, surface->id};
			np_window_event_send(server, NP_GUEST_TOPLEVEL_CREATED, fields, 2);
		} else if (np_surface_is_popup(surface)) {
			uint32_t fields[] = {
				surface->window_id, surface->id, surface->popup_parent_window,
				(uint32_t)surface->popup_x, (uint32_t)surface->popup_y,
				(uint32_t)surface->popup_width, (uint32_t)surface->popup_height,
			};
			np_window_event_send(server, NP_GUEST_POPUP_CREATED, fields, 7);
		}

		if (surface->title) {
			body = cJSON_CreateObject();
			cJSON_AddNumberToObject(body, "window", surface->window_id);
			cJSON_AddStringToObject(body, "title", surface->title);
			np_host_send(&server->host, "titleChanged", body);
		}
		if (surface->app_id) {
			body = cJSON_CreateObject();
			cJSON_AddNumberToObject(body, "window", surface->window_id);
			cJSON_AddStringToObject(body, "appID", surface->app_id);
			np_host_send(&server->host, "appIDChanged", body);
		}
		if (surface->decoration_negotiated) {
			body = cJSON_CreateObject();
			cJSON_AddNumberToObject(body, "window", surface->window_id);
			cJSON_AddBoolToObject(body, "serverSide", surface->decoration_server_side);
			np_host_send(&server->host, "decorationModeChanged", body);
		}
	}

	/* A new host has no decoder history or media resources. Re-encode every
	 * current surface as an IDR and rebuild the same window scene graph used by
	 * the VM transport. Sending stale resource ids would leave the host waiting
	 * forever for frames that belonged to the previous TCP connection. */
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		(void)np_presentation_republish_remote(surface);
	}
}

static void discard_disconnected_host_reads(struct np_server *server) {
	(void)server;
}

static void sync_one_host_source(
	struct np_server *server, struct np_host *host,
	struct wl_event_source **source, int *watched_fd, uint32_t *watched_mask,
	wl_event_loop_fd_func_t callback)
{
	/* Writability is watched only while this stream has queued bytes. */
	uint32_t mask = WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR;
	if (np_host_has_backlog(host)) mask |= WL_EVENT_WRITABLE;

	if (*watched_fd == host->conn_fd) {
		if (*source && *watched_mask != mask) {
			wl_event_source_fd_update(*source, mask);
			*watched_mask = mask;
		}
		return;
	}
	if (*source) {
		wl_event_source_remove(*source);
		*source = NULL;
	}
	*watched_fd = host->conn_fd;
	*watched_mask = mask;
	if (host->conn_fd >= 0) {
		struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
		*source = wl_event_loop_add_fd(
			loop, host->conn_fd, mask, callback, server);
	}
}

static bool all_host_channels_connected(struct np_server *server)
{
	if (!np_host_connected(&server->host)) return false;
	return np_media_connected(&server->media);
}

static void close_host_session(struct np_server *server)
{
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
	np_host_session_sync(server);
	return 0;
}

static void sync_media_source(struct np_server *server)
{
	int fd = np_media_connection_fd(&server->media);
	if (server->watched_media_fd == fd) return;
	if (server->media_connection_source) {
		wl_event_source_remove(server->media_connection_source);
		server->media_connection_source = NULL;
	}
	server->watched_media_fd = fd;
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
		server->host_session_ready = true;
		cJSON *ready = cJSON_CreateObject();
		cJSON_AddNumberToObject(ready, "sessionID", (double)(uint32_t)getpid());
		cJSON_AddNumberToObject(ready, "protocolVersion", 2);
		np_host_send(&server->host, "channelReady", ready);
		republish_state(server);
	}

	sync_one_host_source(
		server, &server->host, &server->host_connection_source,
		&server->watched_host_fd, &server->watched_host_mask,
		host_channel_readable);
	sync_media_source(server);
}

static int host_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&server->host, np_input_handle_host_command, np_input_handle_host_binary, server);
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
void np_host_session_reset_readiness(void)
{
	unpublish_session_environment();
}

bool np_host_session_listen(struct np_server *server)
{
	return np_host_listen_tcp(&server->host, NP_SURFACE_PORT) &&
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
}

void np_host_session_pump(struct np_server *server)
{
	np_host_pump(&server->host, np_input_handle_host_command,
	             np_input_handle_host_binary, server);
	np_media_pump(&server->media);
	np_media_accept(&server->media);
	handle_media_attachment(server);
}

void np_host_session_finish(struct np_server *server)
{
	np_media_finish(&server->media);
	np_host_finish(&server->host);
	unpublish_session_environment();
}
