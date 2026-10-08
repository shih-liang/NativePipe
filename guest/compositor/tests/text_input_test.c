/* Real text-input-v3 resources and event serialization. Only the host wire
 * sink and surface lookup are fixtures; focus, pending state and UTF-8 bounds
 * run through the production handlers. */
#include "../text_input.c"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static struct { uint8_t opcode; uint32_t window, epoch, hints, purpose, cause; bool enabled; } sent[32];
static size_t sent_count;

struct np_surface *np_surface_by_window(struct np_server *server, uint32_t window) {
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link)
		if (surface->window_id == window) return surface;
	return NULL;
}
bool np_window_event_send_message(struct np_server *server, struct np_window_message *message) {
	assert(sent_count < sizeof(sent) / sizeof(sent[0]));
	struct np_window_reader reader;
	assert(np_window_reader_init(&reader, message->data, message->len, NP_WINDOW_GUEST_TO_HOST));
	sent[sent_count].opcode = reader.opcode;
	sent[sent_count].window = np_window_read_u32(&reader);
	if (reader.opcode == NP_GUEST_TEXT_INPUT_ENABLED) {
		sent[sent_count].epoch = np_window_read_u32(&reader);
		sent[sent_count].enabled = np_window_read_bool(&reader);
	}
	if (reader.opcode == NP_GUEST_TEXT_INPUT_CONTENT_TYPE) {
		sent[sent_count].hints = np_window_read_u32(&reader);
		sent[sent_count].purpose = np_window_read_u32(&reader);
		sent[sent_count].cause = np_window_read_u32(&reader);
		assert(np_window_reader_finished(&reader));
	}
	sent_count++;
	return true;
}
static uint32_t word(const unsigned char *bytes) { uint32_t value; memcpy(&value, bytes, 4); return value; }
static unsigned drain(struct np_server *server, int peer, uint32_t object, uint16_t opcode, uint32_t *last) {
	wl_display_flush_clients(server->display);
	unsigned char bytes[8192]; unsigned count = 0;
	for (;;) {
		ssize_t length = recv(peer, bytes, sizeof(bytes), MSG_DONTWAIT);
		if (length < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK); break; }
		if (!length) break;
		for (size_t offset = 0; offset < (size_t)length;) {
			assert(offset + 8 <= (size_t)length);
			uint32_t header = word(bytes + offset + 4); size_t size = header >> 16;
			assert(size >= 8 && offset + size <= (size_t)length);
			if (word(bytes + offset) == object && (uint16_t)header == opcode) {
				count++; if (last && size >= 12) *last = word(bytes + offset + 8);
			}
			offset += size;
		}
	}
	return count;
}
static struct wl_client *make_client(struct np_server *server, int *peer) {
	int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
	struct wl_client *client = wl_client_create(server->display, pair[0]); assert(client);
	*peer = pair[1];
	np_text_input_manager_bind(client, server, 1, 2);
	assert(wl_client_get_object(client, 2));
	return client;
}
static void make_surface(struct np_server *server, struct np_surface *surface,
                         struct wl_client *client, uint32_t id, uint32_t window) {
	memset(surface, 0, sizeof(*surface)); surface->server = server;
	surface->id = id; surface->window_id = window;
	surface->resource = wl_resource_create(client, &wl_surface_interface, 6, id); assert(surface->resource);
	wl_resource_set_implementation(surface->resource, NULL, surface, NULL);
	wl_list_insert(server->surfaces.prev, &surface->link);
}
static struct np_text_input *make_input(struct wl_client *client, uint32_t id) {
	text_input_manager_get(client, wl_client_get_object(client, 2), id, NULL);
	struct wl_resource *resource = wl_client_get_object(client, id); assert(resource);
	return wl_resource_get_user_data(resource);
}
static void focus(struct np_server *server, struct np_surface *old, struct np_surface *next) {
	server->focused_window = next ? next->window_id : 0;
	np_text_input_focus_changed(server, old, next);
}
int main(void) {
	struct np_server server = {0}; server.display = wl_display_create(); assert(server.display);
	wl_list_init(&server.text_inputs); wl_list_init(&server.surfaces);
	int first_peer, second_peer;
	struct wl_client *first = make_client(&server, &first_peer), *second = make_client(&server, &second_peer);
	struct np_surface a, b, other;
	make_surface(&server, &a, first, 3, 1); make_surface(&server, &b, first, 4, 2);
	make_surface(&server, &other, second, 3, 3);
	struct np_text_input *input = make_input(first, 5), *background = make_input(second, 4);
	focus(&server, NULL, &a);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_ENTER, NULL) == 1);
	assert(drain(&server, second_peer, 4, ZWP_TEXT_INPUT_V3_ENTER, NULL) == 0);

	/* A background client cannot enable or update the active host input field. */
	text_input_enable(second, background->resource);
	text_input_set_surrounding_text(second, background->resource, "bad", 3, 3);
	text_input_set_content_type(second, background->resource, 0x80, 8);
	text_input_commit(second, background->resource);
	assert(!background->enabled && !background->pending_surrounding && sent_count == 0);

	text_input_enable(first, input->resource);
	text_input_set_content_type(first, input->resource, 0x100, 8);
	text_input_set_text_change_cause(first, input->resource, 1);
	text_input_set_cursor_rectangle(first, input->resource, 4, 5, 1, 20);
	text_input_set_surrounding_text(first, input->resource, "ab中😀z", 5, 5);
	assert(!input->enabled && !input->surrounding && sent_count == 0);
	text_input_commit(first, input->resource);
	assert(input->enabled && !strcmp(input->surrounding, "ab中😀z"));
	assert(sent_count == 4 && sent[0].opcode == NP_GUEST_TEXT_INPUT_ENABLED && sent[0].enabled);
	assert(input->epoch && sent[0].epoch == input->epoch);
	assert(sent[1].opcode == NP_GUEST_TEXT_INPUT_CONTENT_TYPE && sent[1].window == 1);
	assert(sent[1].hints == 0x100 && sent[1].purpose == 8 && sent[1].cause == 1);
	assert(sent[2].opcode == NP_GUEST_TEXT_INPUT_CURSOR_RECT);
	assert(sent[3].opcode == NP_GUEST_TEXT_INPUT_SURROUNDING_TEXT);

	/* Only one text-input object owns the keyboard's enabled field. */
	struct np_text_input *duplicate = make_input(first, 6);
	text_input_enable(first, duplicate->resource); text_input_commit(first, duplicate->resource);
	assert(!duplicate->enabled);
	size_t before_duplicate_disable = sent_count;
	text_input_disable(first, duplicate->resource); text_input_commit(first, duplicate->resource);
	assert(!duplicate->enabled && input->enabled && sent_count == before_duplicate_disable);
	drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL);
	np_text_input_deliver(&server, 3, input->epoch, "wrong", NULL, 0, 0, 0, 0);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 0);
	assert(drain(&server, second_peer, 4, ZWP_TEXT_INPUT_V3_DONE, NULL) == 0);

	/* One atomic batch carries reconversion and exactly one matching serial. */
	uint32_t serial = 0;
	np_text_input_deliver(&server, 1, input->epoch, "文", "", 0, 0, 3, 4);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, &serial) == 1);
	assert(serial == input->serial);
	assert(drain(&server, second_peer, 4, ZWP_TEXT_INPUT_V3_DONE, NULL) == 0);
	for (unsigned invalid = 0; invalid < 4; invalid++) {
		if (invalid == 0) np_text_input_deliver(&server, 1, input->epoch, NULL, "😀", 1, 1, 0, 0);
		if (invalid == 1) np_text_input_deliver(&server, 1, input->epoch, NULL, "😀", -1, 0, 0, 0);
		if (invalid == 2) np_text_input_deliver(&server, 1, input->epoch, NULL, NULL, 0, 0, 1, 0);
		if (invalid == 3) np_text_input_deliver(&server, 1, input->epoch, NULL, NULL, 0, 0, UINT32_MAX, 0);
		assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 0);
	}
	np_text_input_deliver(&server, 1, input->epoch, NULL, "😀", -1, -1, 0, 0);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 1);

	/* Normal commits retain an epoch; a same-window field enable retires it. */
	uint32_t old_epoch = input->epoch;
	text_input_set_surrounding_text(first, input->resource, "changed", 7, 7);
	text_input_commit(first, input->resource);
	assert(input->epoch == old_epoch);
	np_text_input_deliver(&server, 1, old_epoch, "a", NULL, 0, 0, 0, 0);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 1);
	text_input_enable(first, input->resource);
	text_input_commit(first, input->resource);
	assert(input->epoch != old_epoch && input->epoch != 0);
	np_text_input_deliver(&server, 1, old_epoch, "late old field", NULL, 0, 0, 0, 0);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 0);
	np_text_input_deliver(&server, 1, input->epoch, "new field", NULL, 0, 0, 0, 0);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 1);

	/* leave must clear pending enable/context, even before its first commit. */
	text_input_enable(first, input->resource);
	text_input_set_surrounding_text(first, input->resource, "pending", 7, 7);
	focus(&server, &a, &other);
	assert(!input->enabled && !input->pending_enabled && !input->pending_reset);
	assert(!input->pending_surrounding && !input->surrounding && !input->entered_surface);
	sent_count = 0;
	text_input_commit(first, input->resource); assert(sent_count == 0);
	focus(&server, &other, &b);
	assert(input->entered_surface == b.id && input->entered_window == 2);
	text_input_enable(first, input->resource); text_input_commit(first, input->resource);
	assert(sent_count == 2 && sent[0].window == 2 && sent[1].hints == 0 && sent[1].purpose == 0);
	drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL);
	np_text_input_deliver(&server, 1, input->epoch, "old window", NULL, 0, 0, 0, 0);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 0);
	np_text_input_deliver(&server, 2, input->epoch, "中", NULL, 0, 0, 0, 0);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 1);

	/* A fresh field without surrounding-text support cannot reconvert stale text. */
	assert(!input->surrounding);
	np_text_input_deliver(&server, 2, input->epoch, "中", NULL, 0, 0, 1, 0);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 0);
	text_input_disable(first, input->resource); text_input_commit(first, input->resource);
	assert(!input->enabled && !input->surrounding);

	/* Destroyed previous surfaces are absent from the focus lookup. The new
	 * enter must reset applied state even though no previous pointer survives. */
	text_input_enable(first, input->resource);
	text_input_set_surrounding_text(first, input->resource, "old context", 11, 11);
	text_input_commit(first, input->resource);
	old_epoch = input->epoch;
	wl_resource_destroy(b.resource); b.resource = NULL; wl_list_remove(&b.link);
	assert(!np_surface_by_window(&server, server.focused_window));
	struct np_surface replacement;
	make_surface(&server, &replacement, first, 7, 4);
	focus(&server, NULL, &replacement);
	assert(!input->enabled && !input->epoch && !input->surrounding && !input->pending_surrounding);
	assert(input->entered_surface == replacement.id && input->entered_window == 4);
	text_input_enable(first, input->resource); text_input_commit(first, input->resource);
	assert(input->epoch != old_epoch);
	drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL);
	np_text_input_deliver(&server, 4, old_epoch, "late destroyed field", NULL, 0, 0, 0, 0);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 0);

	/* Disconnect can follow destruction before any replacement enter. Both
	 * the applied field and its queued next-field requests must be retired. */
	text_input_set_surrounding_text(first, input->resource, "snapshot", 8, 8);
	text_input_commit(first, input->resource);
	text_input_enable(first, input->resource);
	text_input_set_surrounding_text(first, input->resource, "pending", 7, 7);
	assert(input->enabled && input->surrounding && input->pending_reset && input->pending_surrounding);
	old_epoch = input->epoch;
	uint32_t epoch_counter = server.text_input_epoch;
	wl_resource_destroy(replacement.resource); replacement.resource = NULL;
	wl_list_remove(&replacement.link);
	assert(!np_surface_by_window(&server, server.focused_window));
	focus(&server, NULL, NULL);
	assert(!input->enabled && !input->epoch && !input->pending_enabled && !input->pending_reset);
	assert(!input->surrounding && !input->pending_surrounding && !input->entered_surface && !input->entered_window);
	assert(server.text_input_epoch == epoch_counter);
	size_t after_disconnect = sent_count;
	focus(&server, NULL, NULL);
	assert(sent_count == after_disconnect && server.text_input_epoch == epoch_counter);
	struct np_surface reconnected;
	make_surface(&server, &reconnected, first, 8, 5);
	focus(&server, NULL, &reconnected);
	text_input_enable(first, input->resource); text_input_commit(first, input->resource);
	assert(input->epoch && input->epoch != old_epoch && server.text_input_epoch != epoch_counter);
	drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL);
	np_text_input_deliver(&server, 5, old_epoch, "late disconnected field", NULL, 0, 0, 0, 0);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 0);
	np_text_input_deliver(&server, 5, input->epoch, "reconnected field", NULL, 0, 0, 0, 0);
	assert(drain(&server, first_peer, 5, ZWP_TEXT_INPUT_V3_DONE, NULL) == 1);
	/* Fixture surfaces intentionally do not own production destroy callbacks. */
	server.focused_window = 0;

	wl_display_destroy_clients(server.display);
	close(first_peer); close(second_peer); wl_display_destroy(server.display);
	puts("text-input-v3: focus ownership, atomic reconversion, UTF-8 and field resets passed");
	return 0;
}
