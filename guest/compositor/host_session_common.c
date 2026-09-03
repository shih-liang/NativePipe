#include "host_session_common.h"

#include "compositor_internal.h"
#include "window_events.h"
#include "windowwire.h"
#include "xwayland.h"

static void send_replay_string(
	struct np_server *server, uint8_t opcode, uint32_t window,
	const char *value)
{
	struct np_window_message message;
	np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST, opcode);
	np_window_put_u32(&message, window);
	np_window_put_string(&message, value ? value : "");
	(void)np_window_event_send_message(server, &message);
	np_window_message_clear(&message);
}

void np_host_session_replay_metadata(struct np_server *server)
{
	struct np_surface *surface;

	/* Create every surface before replaying roles and parent references. */
	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		uint32_t fields[] = {surface->id};
		np_window_event_send(server, NP_GUEST_SURFACE_CREATED, fields, 1);
	}

	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		if (np_surface_is_toplevel(surface)) {
			uint32_t fields[] = {surface->window_id, surface->id};
			np_window_event_send(server, NP_GUEST_TOPLEVEL_CREATED, fields, 2);
			struct wl_client *client = wl_resource_get_client(surface->resource);
			np_window_event_send_force_quit_capability(
				server, surface->window_id,
				!np_xwayland_owns_client(server, client));
		} else if (np_surface_is_popup(surface)) {
			uint32_t fields[] = {
				surface->window_id, surface->id,
				surface->popup_parent_window,
				(uint32_t)surface->popup_x, (uint32_t)surface->popup_y,
				(uint32_t)surface->popup_width,
				(uint32_t)surface->popup_height,
			};
			np_window_event_send(server, NP_GUEST_POPUP_CREATED, fields, 7);
		}

		if (surface->title)
			send_replay_string(
				server, NP_GUEST_TITLE_CHANGED,
				surface->window_id, surface->title);
		if (surface->app_id)
			send_replay_string(
				server, NP_GUEST_APP_ID_CHANGED,
				surface->window_id, surface->app_id);
		if (surface->decoration_negotiated) {
			struct np_window_message message;
			np_window_message_init(
				&message, NP_WINDOW_GUEST_TO_HOST,
				NP_GUEST_DECORATION_MODE_CHANGED);
			np_window_put_u32(&message, surface->window_id);
			np_window_put_bool(&message, surface->decoration_server_side);
			(void)np_window_event_send_message(server, &message);
			np_window_message_clear(&message);
		}
	}
}

void np_host_session_watch(
	struct np_server *server, struct np_host *host,
	struct wl_event_source **source, int *watched_fd, uint32_t *watched_mask,
	wl_event_loop_fd_func_t callback)
{
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
