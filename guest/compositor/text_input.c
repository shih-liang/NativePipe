#include "text_input.h"

#include "compositor_internal.h"
#include "hostlink.h"
#include "text-input-v3-server-protocol.h"

#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

struct np_text_input {
	struct wl_list link;
	struct wl_resource *resource;
	struct np_server *server;
	/* Applied state, and the pending half that commit promotes. */
	bool enabled;
	bool pending_enabled;
	bool pending_cursor_set;
	int32_t pending_cursor_x, pending_cursor_y;
	int32_t pending_cursor_width, pending_cursor_height;
	char *pending_surrounding;
	int32_t pending_cursor_index, pending_anchor_index;
	/* Echoed back in done so clients can discard obsolete IME events. */
	uint32_t serial;
};

static void text_input_enable(struct wl_client *client, struct wl_resource *resource) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (input) input->pending_enabled = true;
}

static void text_input_disable(struct wl_client *client, struct wl_resource *resource) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (input) input->pending_enabled = false;
}

static void text_input_set_surrounding_text(struct wl_client *client,
                                            struct wl_resource *resource,
                                            const char *text, int32_t cursor,
                                            int32_t anchor) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input) return;
	free(input->pending_surrounding);
	input->pending_surrounding = strdup(text ? text : "");
	input->pending_cursor_index = cursor;
	input->pending_anchor_index = anchor;
}

static void text_input_set_text_change_cause(struct wl_client *client,
                                             struct wl_resource *resource, uint32_t cause) {
	// Only distinguishes IME-caused edits from other ones, which matters to an
	// input method running inside the compositor. Ours runs on the Mac.
}

static void text_input_set_content_type(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t hint, uint32_t purpose) {
	// Password and digit fields would map onto NSTextInputContext hints; v1
	// leaves the Mac's default behaviour alone rather than guessing at it.
}

static void text_input_set_cursor_rectangle(struct wl_client *client,
                                            struct wl_resource *resource,
                                            int32_t x, int32_t y, int32_t width, int32_t height) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input) return;
	input->pending_cursor_set = true;
	input->pending_cursor_x = x;
	input->pending_cursor_y = y;
	input->pending_cursor_width = width;
	input->pending_cursor_height = height;
}

static void text_input_commit(struct wl_client *client, struct wl_resource *resource) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input) return;
	struct np_server *server = input->server;
	input->serial++;

	bool was_enabled = input->enabled;
	input->enabled = input->pending_enabled;

	if (was_enabled != input->enabled) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "window", server->focused_window);
		cJSON_AddBoolToObject(body, "enabled", input->enabled);
		np_host_send(&server->host, "textInputEnabled", body);
	}
	if (input->enabled && input->pending_cursor_set) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "window", server->focused_window);
		cJSON_AddNumberToObject(body, "x", input->pending_cursor_x);
		cJSON_AddNumberToObject(body, "y", input->pending_cursor_y);
		cJSON_AddNumberToObject(body, "width", input->pending_cursor_width);
		cJSON_AddNumberToObject(body, "height", input->pending_cursor_height);
		np_host_send(&server->host, "textInputCursorRect", body);
		input->pending_cursor_set = false;
	}
	if (input->enabled && input->pending_surrounding) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "window", server->focused_window);
		cJSON_AddStringToObject(body, "text", input->pending_surrounding);
		cJSON_AddNumberToObject(body, "cursor", input->pending_cursor_index);
		cJSON_AddNumberToObject(body, "anchor", input->pending_anchor_index);
		np_host_send(&server->host, "textInputSurroundingText", body);
		free(input->pending_surrounding);
		input->pending_surrounding = NULL;
	}
}

static void text_input_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_text_input_v3_interface text_input_implementation = {
	.destroy = text_input_destroy_handler,
	.enable = text_input_enable,
	.disable = text_input_disable,
	.set_surrounding_text = text_input_set_surrounding_text,
	.set_text_change_cause = text_input_set_text_change_cause,
	.set_content_type = text_input_set_content_type,
	.set_cursor_rectangle = text_input_set_cursor_rectangle,
	.commit = text_input_commit,
};

static void text_input_resource_destroy(struct wl_resource *resource) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input) return;
	if (input->enabled) {
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "window", input->server->focused_window);
		cJSON_AddBoolToObject(body, "enabled", false);
		np_host_send(&input->server->host, "textInputEnabled", body);
	}
	wl_list_remove(&input->link);
	free(input->pending_surrounding);
	free(input);
}

/// Tells each client's text input whether the focused surface is now theirs.
/// v3 requires enter/leave to track keyboard focus exactly.
void np_text_input_focus_changed(struct np_server *server,
                                     struct np_surface *previous,
                                     struct np_surface *next) {
	struct np_text_input *input;
	wl_list_for_each(input, &server->text_inputs, link) {
		struct wl_client *owner = wl_resource_get_client(input->resource);
		if (previous && owner == wl_resource_get_client(previous->resource)) {
			zwp_text_input_v3_send_leave(input->resource, previous->resource);
			// Leaving disables the field, per the protocol. Saying so keeps the
			// Mac's input context from staying armed for a window that is gone.
			if (input->enabled) {
				input->enabled = false;
				input->pending_enabled = false;
				cJSON *body = cJSON_CreateObject();
				cJSON_AddNumberToObject(body, "window",
				                        previous ? previous->window_id : 0);
				cJSON_AddBoolToObject(body, "enabled", false);
				np_host_send(&server->host, "textInputEnabled", body);
			}
		}
		if (next && owner == wl_resource_get_client(next->resource)) {
			zwp_text_input_v3_send_enter(input->resource, next->resource);
		}
	}
}

/// Delivers what the macOS input method produced. Everything in one batch is
/// applied by the trailing `done`, which is what makes a preedit replacement
/// atomic instead of a flicker through the empty string.
void np_text_input_deliver(struct np_server *server, const char *commit_text,
                               const char *preedit_text, int32_t cursor_begin,
                               int32_t cursor_end, int32_t delete_before,
                               int32_t delete_after) {
	struct np_surface *focused = server->focused_window
		? np_surface_by_window(server, server->focused_window) : NULL;
	if (!focused) return;

	struct np_text_input *input;
	wl_list_for_each(input, &server->text_inputs, link) {
		if (!input->enabled) continue;
		if (wl_resource_get_client(input->resource) !=
		    wl_resource_get_client(focused->resource)) continue;
		if (delete_before || delete_after) {
			zwp_text_input_v3_send_delete_surrounding_text(
				input->resource, (uint32_t)delete_before, (uint32_t)delete_after);
		}
		if (preedit_text) {
			zwp_text_input_v3_send_preedit_string(
				input->resource, preedit_text[0] ? preedit_text : NULL,
				cursor_begin, cursor_end);
		}
		if (commit_text && commit_text[0]) {
			zwp_text_input_v3_send_commit_string(input->resource, commit_text);
		}
		zwp_text_input_v3_send_done(input->resource, input->serial);
	}
	wl_display_flush_clients(server->display);
}

static void text_input_manager_get(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t id, struct wl_resource *seat) {
	struct np_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *created = wl_resource_create(
		client, &zwp_text_input_v3_interface, wl_resource_get_version(resource), id);
	if (!created) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_text_input *input = calloc(1, sizeof(*input));
	if (!input) {
		wl_resource_destroy(created);
		wl_client_post_no_memory(client);
		return;
	}
	input->resource = created;
	input->server = server;
	wl_list_insert(&server->text_inputs, &input->link);
	wl_resource_set_implementation(created, &text_input_implementation, input,
	                               text_input_resource_destroy);

	// A client that binds while already focused must be told so, or it will sit
	// waiting for an enter that already happened.
	struct np_surface *focused = server->focused_window
		? np_surface_by_window(server, server->focused_window) : NULL;
	if (focused && wl_resource_get_client(focused->resource) == client) {
		zwp_text_input_v3_send_enter(created, focused->resource);
	}
}

static void text_input_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_text_input_manager_v3_interface text_input_manager_implementation = {
	.destroy = text_input_manager_destroy,
	.get_text_input = text_input_manager_get,
};

void np_text_input_manager_bind(struct wl_client *client, void *data,
                                    uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &zwp_text_input_manager_v3_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &text_input_manager_implementation, data, NULL);
}

/// The innermost popup currently holding a grab, or NULL.
///
/// Surfaces are head-inserted, so the first match walking forward is the most
