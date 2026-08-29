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

#ifndef NP_REMOTE
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
#endif

static int host_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&server->host);
	np_host_pump(&server->host, np_input_handle_host_command, np_input_handle_host_binary, server);
	np_presentation_flush(server);
	np_host_session_sync(server);
	wl_display_flush_clients(server->display);
	return 0;
}

#ifndef NP_REMOTE
static int host_control_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&server->host_control);
	np_host_pump(&server->host_control, np_input_handle_host_command, np_input_handle_host_binary, server);
	np_host_session_sync(server);
	return 0;
}

static int host_input_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&server->host_input);
	np_host_pump(&server->host_input, np_input_handle_host_command, np_input_handle_host_binary, server);
	np_host_session_sync(server);
	return 0;
}

static int host_feedback_channel_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	if (mask & WL_EVENT_WRITABLE) np_host_flush(&server->host_feedback);
	np_host_pump(&server->host_feedback, np_input_handle_host_command, np_input_handle_host_binary, server);
	np_host_session_sync(server);
	return 0;
}
#endif

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

#ifndef NP_REMOTE
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
#else
	// Remote output is still an encoded per-surface stream, so replay its latest
	// decoded frame through the ordinary committed envelope.
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		if (!surface->has_published) continue;
		uint32_t resource_id = surface->last_resource_id;
		if (!resource_id) continue;

		cJSON *frame = cJSON_CreateObject();
		cJSON_AddNumberToObject(frame, "resourceID", resource_id);
		cJSON_AddNumberToObject(frame, "width", surface->last_width);
		cJSON_AddNumberToObject(frame, "height", surface->last_height);
		cJSON_AddNumberToObject(frame, "bytesPerRow", surface->last_stride);
		cJSON_AddStringToObject(frame, "format", surface->last_format);
		if (surface->last_source) {
			cJSON_AddStringToObject(frame, "source", surface->last_source);
		}
		if (surface->last_source && strcmp(surface->last_source, "encoded") == 0) {
			cJSON_AddStringToObject(frame, "codec", "h264");
			cJSON_AddNumberToObject(frame, "bitstreamEpoch", surface->last_epoch);
		}
		cJSON_AddNumberToObject(frame, "scale", surface->scale);
		if (surface->geometry_set) {
			cJSON *geometry = cJSON_CreateObject();
			cJSON_AddNumberToObject(geometry, "x", surface->geometry_x);
			cJSON_AddNumberToObject(geometry, "y", surface->geometry_y);
			cJSON_AddNumberToObject(geometry, "width", surface->geometry_width);
			cJSON_AddNumberToObject(geometry, "height", surface->geometry_height);
			cJSON_AddItemToObject(frame, "windowGeometry", geometry);
		}
		np_presentation_add_viewport(surface, frame);
		cJSON_AddItemToObject(frame, "damage", cJSON_CreateArray());

		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "surface", surface->id);
		cJSON_AddItemToObject(body, "frame", frame);

		// Straight into the pending slot, so a commit racing this replay wins.
		if (surface->pending_frame) cJSON_Delete(surface->pending_frame);
		surface->pending_frame = body;
	}
#endif
}

static void discard_disconnected_host_reads(struct np_server *server) {
#ifndef NP_REMOTE
	struct np_surface *surface;
	/* No completion can arrive from the old socket. Releasing these holds is
	 * safe because that host can no longer submit new Metal work from them. */
	wl_list_for_each(surface, &server->surfaces, link)
		np_scene_discard_presentations(surface);
	wl_list_for_each(surface, &server->surfaces, link)
		np_surface_apply_unblocked(surface);
#else
	(void)server;
#endif
}

static void sync_one_host_source(
	struct np_server *server, struct np_host *host,
	struct wl_event_source **source, int *watched_fd, uint32_t *watched_mask,
	wl_event_loop_fd_func_t callback)
{
	/* Writability is watched only while this lane has queued bytes. Each lane has
	 * independent vsock credit, so a blocked feedback stream cannot park control
	 * or input behind the same POLLOUT wait. */
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
#ifndef NP_REMOTE
	return np_host_connected(&server->host_control) &&
	       np_host_connected(&server->host_input) &&
	       np_host_connected(&server->host_feedback);
#else
	return true;
#endif
}

static void close_host_session(struct np_server *server)
{
	np_host_disconnect(&server->host);
#ifndef NP_REMOTE
	np_host_disconnect(&server->host_control);
	np_host_disconnect(&server->host_input);
	np_host_disconnect(&server->host_feedback);
#endif
}

void np_host_session_sync(struct np_server *server) {
	bool connected = all_host_channels_connected(server);
	if (server->host_session_ready && !connected) {
		server->host_session_ready = false;
#ifndef NP_REMOTE
		unpublish_session_environment();
#endif
		discard_disconnected_host_reads(server);
		/* Do not pair a newly dialled lane with sockets from the old generation. */
		close_host_session(server);
		connected = false;
	}

	if (!server->host_session_ready && connected) {
		/* channelReady is the host launch gate. The environment must be visible
		 * before the event because the host may launch immediately on receipt. */
#ifndef NP_REMOTE
		if (!publish_session_environment(server))
			fprintf(stderr, "[wayland] could not publish the session environment\n");
#endif
		server->host_session_ready = true;
		cJSON *ready = cJSON_CreateObject();
		cJSON_AddNumberToObject(ready, "sessionID", (double)(uint32_t)getpid());
#ifdef NP_REMOTE
		cJSON_AddNumberToObject(ready, "protocolVersion", 1);
#else
		cJSON_AddNumberToObject(ready, "protocolVersion", 2);
#endif
		np_host_send(&server->host, "channelReady", ready);
		republish_state(server);
	}

	sync_one_host_source(
		server, &server->host, &server->host_connection_source,
		&server->watched_host_fd, &server->watched_host_mask,
		host_channel_readable);
#ifndef NP_REMOTE
	sync_one_host_source(
		server, &server->host_control, &server->host_control_connection_source,
		&server->watched_host_control_fd, &server->watched_host_control_mask,
		host_control_channel_readable);
	sync_one_host_source(
		server, &server->host_input, &server->host_input_connection_source,
		&server->watched_host_input_fd, &server->watched_host_input_mask,
		host_input_channel_readable);
	sync_one_host_source(
		server, &server->host_feedback, &server->host_feedback_connection_source,
		&server->watched_host_feedback_fd, &server->watched_host_feedback_mask,
		host_feedback_channel_readable);
#endif
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

#ifndef NP_REMOTE
static int host_control_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&server->host_control, np_input_handle_host_command, np_input_handle_host_binary, server);
	np_host_session_sync(server);
	return 0;
}

static int host_input_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&server->host_input, np_input_handle_host_command, np_input_handle_host_binary, server);
	np_host_session_sync(server);
	return 0;
}

static int host_feedback_listener_readable(int fd, uint32_t mask, void *data) {
	struct np_server *server = data;
	(void)fd;
	(void)mask;
	np_host_pump(&server->host_feedback, np_input_handle_host_command, np_input_handle_host_binary, server);
	np_host_session_sync(server);
	return 0;
}
#endif

#ifdef NP_REMOTE
static int media_listener_readable(int fd, uint32_t mask, void *data) {
	(void)fd;
	(void)mask;
	struct np_server *server = data;
	np_media_accept(&server->media);
	return 0;
}
#endif
void np_host_session_reset_readiness(void)
{
#ifndef NP_REMOTE
	unpublish_session_environment();
#endif
}

bool np_host_session_listen(struct np_server *server)
{
#ifdef NP_REMOTE
	return np_host_listen_tcp(&server->host, NP_SURFACE_PORT) &&
	       np_media_listen(&server->media);
#else
	return np_host_listen(&server->host, NP_WINDOW_EVENT_PORT) &&
	       np_host_listen(&server->host_control, NP_WINDOW_CONTROL_PORT) &&
	       np_host_listen(&server->host_input, NP_WINDOW_INPUT_PORT) &&
	       np_host_listen(&server->host_feedback, NP_WINDOW_FEEDBACK_PORT);
#endif
}

bool np_host_session_set_socket(struct np_server *server, const char *socket)
{
	if (!socket) return false;
#ifndef NP_REMOTE
	if (strlen(socket) >= sizeof(server->session_socket)) return false;
	strcpy(server->session_socket, socket);
#else
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !runtime[0]) runtime = "/tmp";
	FILE *env = fopen("/tmp/remotepipe-wayland.env", "w");
	if (env) {
		fprintf(env, "WAYLAND_DISPLAY=%s\nXDG_RUNTIME_DIR=%s\n", socket, runtime);
		fclose(env);
	}
#endif
	return true;
}

void np_host_session_attach(struct np_server *server, struct wl_event_loop *loop)
{
	wl_event_loop_add_fd(loop, server->host.listen_fd, WL_EVENT_READABLE,
	                     host_listener_readable, server);
#ifndef NP_REMOTE
	wl_event_loop_add_fd(loop, server->host_control.listen_fd, WL_EVENT_READABLE,
	                     host_control_listener_readable, server);
	wl_event_loop_add_fd(loop, server->host_input.listen_fd, WL_EVENT_READABLE,
	                     host_input_listener_readable, server);
	wl_event_loop_add_fd(loop, server->host_feedback.listen_fd, WL_EVENT_READABLE,
	                     host_feedback_listener_readable, server);
#else
	if (server->media.listen_fd >= 0)
		wl_event_loop_add_fd(loop, server->media.listen_fd, WL_EVENT_READABLE,
		                     media_listener_readable, server);
#endif
}

void np_host_session_pump(struct np_server *server)
{
	np_host_pump(&server->host, np_input_handle_host_command,
	             np_input_handle_host_binary, server);
#ifndef NP_REMOTE
	np_host_pump(&server->host_control, np_input_handle_host_command,
	             np_input_handle_host_binary, server);
	np_host_pump(&server->host_input, np_input_handle_host_command,
	             np_input_handle_host_binary, server);
	np_host_pump(&server->host_feedback, np_input_handle_host_command,
	             np_input_handle_host_binary, server);
#else
	np_media_pump(&server->media);
	np_media_accept(&server->media);
	if (server->media.just_attached) {
		server->media.just_attached = false;
		struct np_surface *surface;
		wl_list_for_each(surface, &server->surfaces, link) {
			if (surface->encoder) np_encoder_force_keyframe(surface->encoder);
		}
		fprintf(stderr, "[media] requested IDR on all encoders\n");
	}
#endif
}

void np_host_session_finish(struct np_server *server)
{
#ifdef NP_REMOTE
	np_media_finish(&server->media);
#endif
	np_host_finish(&server->host);
#ifndef NP_REMOTE
	np_host_finish(&server->host_control);
	np_host_finish(&server->host_input);
	np_host_finish(&server->host_feedback);
	unpublish_session_environment();
#endif
}
