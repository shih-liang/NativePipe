#include "text_input.h"

#include "compositor_internal.h"
#include "text-input-v3-server-protocol.h"
#include "window_events.h"
#include "windowwire.h"

#include <glib.h>
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
	bool pending_reset;
	uint32_t entered_surface, entered_window;
	uint32_t epoch;
	uint32_t pending_hints, pending_purpose, pending_cause;
	bool pending_content_type;
	char *surrounding;
	int32_t cursor_index, anchor_index;
	bool pending_cursor_set;
	int32_t pending_cursor_x, pending_cursor_y;
	int32_t pending_cursor_width, pending_cursor_height;
	char *pending_surrounding;
	int32_t pending_cursor_index, pending_anchor_index;
	/* Echoed back in done so clients can discard obsolete IME events. */
	uint32_t serial;
};

static void send_message(struct np_server *server,
	                     struct np_window_message *message) {
	(void)np_window_event_send_message(server, message);
	np_window_message_clear(message);
}

static void send_enabled(struct np_server *server, uint32_t window, uint32_t epoch,
	                     bool enabled) {
	struct np_window_message message;
	np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST,
	                       NP_GUEST_TEXT_INPUT_ENABLED);
	np_window_put_u32(&message, window);
	np_window_put_u32(&message, epoch);
	np_window_put_bool(&message, enabled);
	send_message(server, &message);
}

/// The surface this input entered, while it still holds its client's keyboard
/// focus, whether that focus is the user's or the agent's.
static struct np_surface *focused_input(struct np_text_input *input) {
	struct np_surface *surface = np_keyboard_focus(
		input->server, wl_resource_get_client(input->resource));
	return surface && surface->id == input->entered_surface ? surface : NULL;
}

/// Each client has its own keyboard focus, so each may have one enabled field.
static bool other_field_enabled(struct np_text_input *input) {
	struct np_text_input *other;
	wl_list_for_each(other, &input->server->text_inputs, link)
		if (other != input && other->enabled && focused_input(other) &&
		    wl_resource_get_client(other->resource) == wl_resource_get_client(input->resource))
			return true;
	return false;
}

static void clear_pending(struct np_text_input *input) {
	free(input->pending_surrounding);
	input->pending_surrounding = NULL;
	input->pending_cursor_set = false;
	input->pending_hints = input->pending_purpose = input->pending_cause = 0;
	input->pending_content_type = false;
	input->pending_reset = false;
	input->pending_enabled = input->enabled;
}

static void clear_focus_state(struct np_text_input *input) {
	input->enabled = false;
	input->epoch = 0;
	clear_pending(input);
	free(input->surrounding); input->surrounding = NULL;
	input->cursor_index = input->anchor_index = 0;
	input->entered_surface = input->entered_window = 0;
}

static void text_input_enable(struct wl_client *client, struct wl_resource *resource) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input || !focused_input(input) || other_field_enabled(input)) return;
	clear_pending(input);
	input->pending_enabled = true;
	input->pending_reset = true;
}

static void text_input_disable(struct wl_client *client, struct wl_resource *resource) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input || !focused_input(input)) return;
	clear_pending(input);
	input->pending_enabled = false;
	input->pending_reset = true;
}

static bool utf8_index(const char *text, int32_t index) {
	size_t length = strlen(text);
	return index >= 0 && (size_t)index <= length &&
	       ((size_t)index == length || ((unsigned char)text[index] & 0xc0) != 0x80);
}

static void text_input_set_surrounding_text(struct wl_client *client,
                                            struct wl_resource *resource,
                                            const char *text, int32_t cursor,
                                            int32_t anchor) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input || !focused_input(input)) return;
	if (!text || strlen(text) > 4000 || !g_utf8_validate(text, -1, NULL) ||
	    !utf8_index(text, cursor) || !utf8_index(text, anchor)) {
		wl_client_post_implementation_error(client, "invalid UTF-8 surrounding text or byte indices");
		return;
	}
	char *copy = strdup(text);
	if (!copy) { wl_client_post_no_memory(client); return; }
	free(input->pending_surrounding);
	input->pending_surrounding = copy;
	input->pending_cursor_index = cursor;
	input->pending_anchor_index = anchor;
}

static void text_input_set_text_change_cause(struct wl_client *client,
                                             struct wl_resource *resource, uint32_t cause) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input || !focused_input(input)) return;
	if (!zwp_text_input_v3_change_cause_is_valid(cause, 1)) {
		wl_client_post_implementation_error(client, "invalid text change cause");
		return;
	}
	input->pending_cause = cause;
	input->pending_content_type = true;
}

static void text_input_set_content_type(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t hint, uint32_t purpose) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input || !focused_input(input)) return;
	if (!zwp_text_input_v3_content_hint_is_valid(hint, 1) ||
	    !zwp_text_input_v3_content_purpose_is_valid(purpose, 1)) {
		wl_client_post_implementation_error(client, "invalid text content type");
		return;
	}
	input->pending_hints = hint;
	input->pending_purpose = purpose;
	input->pending_content_type = true;
}

static void text_input_set_cursor_rectangle(struct wl_client *client,
                                            struct wl_resource *resource,
                                            int32_t x, int32_t y, int32_t width, int32_t height) {
	struct np_text_input *input = wl_resource_get_user_data(resource);
	if (!input || !focused_input(input) || width < 0 || height < 0) return;
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
	struct np_surface *focused = focused_input(input);
	if (!focused) { clear_pending(input); return; }
	if (input->pending_enabled && other_field_enabled(input)) {
		clear_pending(input); return;
	}
	/* An inactive object's disable must not turn off another text input from
	 * the same client which still owns this seat's enabled field. */
	if (!input->pending_enabled && !input->enabled) { clear_pending(input); return; }
	bool reset = input->pending_reset;
	bool was_enabled = input->enabled;
	input->enabled = input->pending_enabled;
	if (reset) {
		if (++server->text_input_epoch == 0) ++server->text_input_epoch;
		input->epoch = server->text_input_epoch;
	}
	if (reset || !input->enabled) {
		free(input->surrounding); input->surrounding = NULL;
	}
	if (reset || was_enabled != input->enabled)
		send_enabled(server, focused->window_id, input->epoch, input->enabled);
	if (!input->enabled) { clear_pending(input); return; }
	if (reset || input->pending_content_type) {
		struct np_window_message message;
		np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST,
		                       NP_GUEST_TEXT_INPUT_CONTENT_TYPE);
		np_window_put_u32(&message, focused->window_id);
		np_window_put_u32(&message, input->pending_hints);
		np_window_put_u32(&message, input->pending_purpose);
		np_window_put_u32(&message, input->pending_cause);
		send_message(server, &message);
		input->pending_content_type = false;
	}
	if (input->pending_cursor_set) {
		struct np_window_message message;
		np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST,
		                       NP_GUEST_TEXT_INPUT_CURSOR_RECT);
		np_window_put_u32(&message, focused->window_id);
		np_window_put_i32(&message, input->pending_cursor_x);
		np_window_put_i32(&message, input->pending_cursor_y);
		np_window_put_i32(&message, input->pending_cursor_width);
		np_window_put_i32(&message, input->pending_cursor_height);
		send_message(server, &message);
		input->pending_cursor_set = false;
	}
	if (input->pending_surrounding) {
		free(input->surrounding);
		input->surrounding = input->pending_surrounding;
		input->pending_surrounding = NULL;
		input->cursor_index = input->pending_cursor_index;
		input->anchor_index = input->pending_anchor_index;
		struct np_window_message message;
		np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST,
		                       NP_GUEST_TEXT_INPUT_SURROUNDING_TEXT);
		np_window_put_u32(&message, focused->window_id);
		np_window_put_string(&message, input->surrounding);
		np_window_put_i32(&message, input->cursor_index);
		np_window_put_i32(&message, input->anchor_index);
		send_message(server, &message);
	}
	input->pending_reset = false;
	input->pending_cause = 0;
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
	if (input->enabled && focused_input(input))
		send_enabled(input->server, input->entered_window, input->epoch, false);
	free(input->surrounding);
	wl_list_remove(&input->link);
	free(input->pending_surrounding);
	free(input);
}

/// Moves each text input to its client's current keyboard focus. v3 requires
/// enter/leave to track keyboard focus exactly.
void np_text_input_focus_changed(struct np_server *server) {
	struct np_text_input *input;
	wl_list_for_each(input, &server->text_inputs, link) {
		struct np_surface *next = np_keyboard_focus(server, wl_resource_get_client(input->resource));
		if (input->entered_surface == (next ? next->id : 0)) continue;
		struct np_surface *previous = input->entered_window
			? np_surface_by_window(server, input->entered_window) : NULL;
		if (previous && previous->id == input->entered_surface && previous->resource &&
		    wl_resource_get_client(previous->resource) == wl_resource_get_client(input->resource))
			zwp_text_input_v3_send_leave(input->resource, previous->resource);
		/* A destroyed surface gets no leave, but its field still retires with
		 * its unapplied requests; a fresh enter also cannot inherit that state. */
		if (input->enabled && input->entered_window)
			send_enabled(server, input->entered_window, input->epoch, false);
		clear_focus_state(input);
		if (next) {
			input->entered_surface = next->id;
			input->entered_window = next->window_id;
			zwp_text_input_v3_send_enter(input->resource, next->resource);
		}
	}
}

bool np_text_input_valid_text(const char *text) {
	return text && g_utf8_validate(text, -1, NULL);
}

/// Agent text goes straight to the enabled field of the agent's focus. The
/// host's input-method epoch belongs to the user's typing and is not involved.
bool np_text_input_commit_agent(struct np_server *server, struct np_surface *focus,
                                const char *text) {
	struct np_text_input *input;
	wl_list_for_each(input, &server->text_inputs, link) {
		if (!input->enabled || !focus || focused_input(input) != focus) continue;
		zwp_text_input_v3_send_commit_string(input->resource, text);
		zwp_text_input_v3_send_done(input->resource, input->serial);
		wl_display_flush_clients(server->display);
		return true;
	}
	return false;
}

/// Delivers what the macOS input method produced. Everything in one batch is
/// applied by the trailing `done`, which is what makes a preedit replacement
/// atomic instead of a flicker through the empty string.
void np_text_input_deliver(struct np_server *server, uint32_t window, uint32_t epoch,
                               const char *commit_text, const char *preedit_text,
                               int32_t cursor_begin, int32_t cursor_end,
                               uint32_t delete_before, uint32_t delete_after) {
	struct np_surface *focus = window && server->focused_window == window
		? np_surface_by_window(server, window) : NULL;
	if (!focus || !epoch) return;
	if ((commit_text && !g_utf8_validate(commit_text, -1, NULL)) ||
	    (preedit_text && (!g_utf8_validate(preedit_text, -1, NULL) ||
	      !((cursor_begin == -1 && cursor_end == -1) ||
	        (utf8_index(preedit_text, cursor_begin) && utf8_index(preedit_text, cursor_end))))))
		return;
	struct np_text_input *input;
	wl_list_for_each(input, &server->text_inputs, link) {
		if (!input->enabled || input->epoch != epoch || focused_input(input) != focus) continue;
		if (delete_before || delete_after) {
			if (!input->surrounding) continue;
			int32_t begin = input->cursor_index < input->anchor_index
				? input->cursor_index : input->anchor_index;
			int32_t end = input->cursor_index > input->anchor_index
				? input->cursor_index : input->anchor_index;
			if (delete_before > (uint32_t)begin ||
			    delete_after > strlen(input->surrounding) - (size_t)end ||
			    !utf8_index(input->surrounding, begin - (int32_t)delete_before) ||
			    !utf8_index(input->surrounding, end + (int32_t)delete_after)) continue;
			zwp_text_input_v3_send_delete_surrounding_text(
				input->resource, delete_before, delete_after);
		}
		if (preedit_text)
			zwp_text_input_v3_send_preedit_string(input->resource,
				preedit_text[0] ? preedit_text : NULL, cursor_begin, cursor_end);
		if (commit_text && commit_text[0])
			zwp_text_input_v3_send_commit_string(input->resource, commit_text);
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
	struct np_surface *focused = np_keyboard_focus(server, client);
	if (focused) {
		input->entered_surface = focused->id;
		input->entered_window = focused->window_id;
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
