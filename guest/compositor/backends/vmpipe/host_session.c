// Host transport lanes, reconnect replay and session readiness.

#define _GNU_SOURCE

#include "backend.h"
#include "backend_internal.h"
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

static bool publish_session_environment(struct np_server *server)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *socket = server->session_socket;
	if (!runtime || !runtime[0] || !socket[0]) return false;

	char path[1024];
	char temporary[1088];
	snprintf(path, sizeof(path), "%s/nativepipe-wayland.env", runtime);
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
	bool ok = fprintf(env, "WAYLAND_DISPLAY=%s\n", socket) > 0;
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
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !runtime[0]) return;
	char path[1024];
	snprintf(path, sizeof(path), "%s/nativepipe-wayland.env", runtime);
	if (unlink(path) < 0 && errno != ENOENT)
		fprintf(stderr, "[wayland] could not remove session readiness: %s\n",
		        strerror(errno));
}

static int host_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&np_vmpipe_backend(server)->host);
	np_host_pump(&np_vmpipe_backend(server)->host, np_input_handle_host_binary, server);
	np_presentation_flush(server);
	np_backend_session_sync(server);
	wl_display_flush_clients(server->display);
	return 0;
}

static int host_control_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&np_vmpipe_backend(server)->host_control);
	np_host_pump(&np_vmpipe_backend(server)->host_control, np_input_handle_host_binary, server);
	np_backend_session_sync(server);
	return 0;
}

static int host_input_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&np_vmpipe_backend(server)->host_input);
	np_host_pump(&np_vmpipe_backend(server)->host_input, np_input_handle_host_binary, server);
	np_backend_session_sync(server);
	return 0;
}

static int host_feedback_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&np_vmpipe_backend(server)->host_feedback);
	np_host_pump(&np_vmpipe_backend(server)->host_feedback, np_input_handle_host_binary, server);
	np_backend_session_sync(server);
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

	/* Rebuild each current xdg scene from its retained source textures. Replaying
	 * the old one-surface `committed` envelope would bypass subsurface ordering,
	 * viewport/clip state and the new host renderer. All callbacks and FIFO
	 * barriers outstanding from the lost host are rebound to this new latch. */
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		struct np_surface *root = np_scene_root(surface);
		if (root != surface || !surface->has_published ||
		    (!surface->current_gpu && !surface->current_shm))
			continue;
		uint32_t presentation_id = np_presentation_next_id(server);
		struct np_surface *member;
		wl_list_for_each(member, &server->surfaces, link) {
			if (np_scene_root(member) != root) continue;
			struct np_frame_callback *callback;
			wl_list_for_each(callback, &member->pending_frame_callbacks, link) {
				if (callback->presentation_id)
					callback->presentation_id = presentation_id;
			}
			if (member->fifo_barrier_active)
				member->fifo_barrier_presentation_id = presentation_id;
		}
		root->scene_presentation_id = presentation_id;
		root->scene_dirty = true;
		root->scene_full_damage = true;
	}

	/* Cursor and drag-icon surfaces have no xdg root, so they retain the small
	 * metadata envelope. np_presentation_queue_last establishes a new host-read
	 * hold for the retained texture before it is sent. */
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		if (np_scene_root(surface) || !surface->has_published ||
		    (!surface->current_gpu && !surface->current_shm))
			continue;
		uint32_t presentation_id = np_presentation_next_id(server);
		struct np_frame_callback *callback;
		wl_list_for_each(callback, &surface->pending_frame_callbacks, link) {
			if (callback->presentation_id)
				callback->presentation_id = presentation_id;
		}
		if (surface->fifo_barrier_active)
			surface->fifo_barrier_presentation_id = presentation_id;
		(void)np_presentation_queue_last(surface, presentation_id);
	}
}

static void discard_disconnected_host_reads(struct np_server *server) {
	struct np_surface *surface;
	/* No completion can arrive from the old socket. Releasing these holds is
	 * safe because that host can no longer submit new Metal work from them. */
	wl_list_for_each(surface, &server->surfaces, link)
		np_scene_discard_presentations(surface);
	wl_list_for_each(surface, &server->surfaces, link)
		np_surface_apply_unblocked(surface);
	np_data_host_disconnected(server);
}

static bool all_host_channels_connected(struct np_server *server)
{
	if (!np_host_connected(&np_vmpipe_backend(server)->host)) return false;
	return np_host_connected(&np_vmpipe_backend(server)->host_control) &&
	       np_host_connected(&np_vmpipe_backend(server)->host_input) &&
	       np_host_connected(&np_vmpipe_backend(server)->host_feedback);
}

static void close_host_session(struct np_server *server)
{
	np_host_disconnect(&np_vmpipe_backend(server)->host);
	np_host_disconnect(&np_vmpipe_backend(server)->host_control);
	np_host_disconnect(&np_vmpipe_backend(server)->host_input);
	np_host_disconnect(&np_vmpipe_backend(server)->host_feedback);
}

void np_backend_session_sync(struct np_server *server) {
	bool connected = all_host_channels_connected(server);
	if (server->host_session_ready && !connected) {
		server->host_session_ready = false;
		unpublish_session_environment();
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
		uint32_t ready[] = {
			(uint32_t)getpid(), NP_WINDOW_PROTOCOL_VERSION,
		};
		np_window_event_send(server, NP_GUEST_SESSION_STARTED, ready, 2);
		republish_state(server);
	}

	np_host_session_watch(
		server, &np_vmpipe_backend(server)->host, &np_vmpipe_backend(server)->host_connection_source,
		&np_vmpipe_backend(server)->watched_host_fd, &np_vmpipe_backend(server)->watched_host_mask,
		host_channel_readable);
	np_host_session_watch(
		server, &np_vmpipe_backend(server)->host_control, &np_vmpipe_backend(server)->host_control_connection_source,
		&np_vmpipe_backend(server)->watched_host_control_fd, &np_vmpipe_backend(server)->watched_host_control_mask,
		host_control_channel_readable);
	np_host_session_watch(
		server, &np_vmpipe_backend(server)->host_input, &np_vmpipe_backend(server)->host_input_connection_source,
		&np_vmpipe_backend(server)->watched_host_input_fd, &np_vmpipe_backend(server)->watched_host_input_mask,
		host_input_channel_readable);
	np_host_session_watch(
		server, &np_vmpipe_backend(server)->host_feedback, &np_vmpipe_backend(server)->host_feedback_connection_source,
		&np_vmpipe_backend(server)->watched_host_feedback_fd, &np_vmpipe_backend(server)->watched_host_feedback_mask,
		host_feedback_channel_readable);
}

static int host_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&np_vmpipe_backend(server)->host, np_input_handle_host_binary, server);
	np_presentation_flush(server);
	np_backend_session_sync(server);
	wl_display_flush_clients(server->display);
	return 0;
}

static int host_control_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&np_vmpipe_backend(server)->host_control, np_input_handle_host_binary, server);
	np_backend_session_sync(server);
	return 0;
}

static int host_input_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&np_vmpipe_backend(server)->host_input, np_input_handle_host_binary, server);
	np_backend_session_sync(server);
	return 0;
}

static int host_feedback_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&np_vmpipe_backend(server)->host_feedback, np_input_handle_host_binary, server);
	np_backend_session_sync(server);
	return 0;
}

void np_backend_session_reset_readiness(void)
{
	unpublish_session_environment();
}

bool np_backend_session_listen(struct np_server *server)
{
	return np_host_listen(&np_vmpipe_backend(server)->host, NP_WINDOW_EVENT_PORT) &&
	       np_host_listen(&np_vmpipe_backend(server)->host_control, NP_WINDOW_CONTROL_PORT) &&
	       np_host_listen(&np_vmpipe_backend(server)->host_input, NP_WINDOW_INPUT_PORT) &&
	       np_host_listen(&np_vmpipe_backend(server)->host_feedback, NP_WINDOW_FEEDBACK_PORT);
}

bool np_backend_session_set_socket(struct np_server *server, const char *socket)
{
	if (!socket) return false;
	if (strlen(socket) >= sizeof(server->session_socket)) return false;
	strcpy(server->session_socket, socket);
	return true;
}

void np_backend_session_attach(struct np_server *server, struct wl_event_loop *loop)
{
	wl_event_loop_add_fd(loop, np_vmpipe_backend(server)->host.listen_fd, WL_EVENT_READABLE,
	                     host_listener_readable, server);
	wl_event_loop_add_fd(loop, np_vmpipe_backend(server)->host_control.listen_fd, WL_EVENT_READABLE,
	                     host_control_listener_readable, server);
	wl_event_loop_add_fd(loop, np_vmpipe_backend(server)->host_input.listen_fd, WL_EVENT_READABLE,
	                     host_input_listener_readable, server);
	wl_event_loop_add_fd(loop, np_vmpipe_backend(server)->host_feedback.listen_fd, WL_EVENT_READABLE,
	                     host_feedback_listener_readable, server);
}

void np_backend_session_finish(struct np_server *server)
{
	np_host_finish(&np_vmpipe_backend(server)->host);
	np_host_finish(&np_vmpipe_backend(server)->host_control);
	np_host_finish(&np_vmpipe_backend(server)->host_input);
	np_host_finish(&np_vmpipe_backend(server)->host_feedback);
	unpublish_session_environment();
}

bool np_backend_connected(const struct np_server *server)
{
	struct np_vmpipe_backend *backend = np_vmpipe_backend(server);
	return backend && np_host_connected(&backend->host);
}

bool np_backend_send_binary(
	struct np_server *server, const void *payload, size_t length)
{
	struct np_vmpipe_backend *backend = np_vmpipe_backend(server);
	return backend && np_host_send_binary(&backend->host, payload, length);
}
