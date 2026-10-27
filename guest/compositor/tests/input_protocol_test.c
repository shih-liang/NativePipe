#define _GNU_SOURCE
#include "../input.c"
#include "../windowwire.c"
#include <assert.h>
#include <sys/socket.h>

static struct np_surface *fixture_surface;
static int focus_resets, activation_resets;
static struct np_surface *fixture_cursor;
static unsigned cursor_changes, cursor_replays, cursor_queues;
static uint32_t cursor_replay_id;
static bool capture_mode;
static bool capture_queue_ok;
static unsigned capture_replays, capture_queues, capture_flushes, capture_member_count;
static unsigned capture_expected_queues;
static uint32_t capture_owner, capture_id;
static struct np_surface *capture_members[3];
static struct np_surface *key_surfaces[8];
static unsigned key_surface_count, key_activations;
struct np_surface *np_surface_by_id(struct np_server *server, uint32_t id)
{
	return fixture_surface && fixture_surface->id == id ? fixture_surface : NULL;
}

struct np_surface *np_surface_by_window(struct np_server *server, uint32_t window)
{
    for (unsigned i = 0; i < key_surface_count; i++)
        if (key_surfaces[i]->window_id == window) return key_surfaces[i];
    return fixture_surface && fixture_surface->window_id == window ? fixture_surface : NULL;
}
void np_data_send_selection(struct np_server *server, struct wl_resource *resource) {}
bool np_surface_assign_role(struct np_surface *surface, enum np_surface_role role)
{
    if (surface->role != NP_SURFACE_ROLE_NONE && surface->role != role) return false;
    surface->role = role;
    return true;
}
struct np_surface *np_scene_root(struct np_surface *surface)
{
    if (surface == fixture_cursor) return NULL;
    while (surface && surface->parent) surface = surface->parent;
    return surface;
}
void np_scale_changed(struct np_surface *surface, int scale) { surface->preferred_scale = scale; }
bool np_xwayland_owns_client(struct np_server *server, struct wl_client *client) { return false; }
bool np_window_event_send(struct np_server *server, uint8_t opcode, const uint32_t *values, size_t count)
{
    if (fixture_cursor) {
        assert(opcode == NP_GUEST_CURSOR_CHANGED && count == 4);
        assert(values[0] == fixture_cursor->id || values[0] == 0);
        cursor_changes++;
    }
    return false;
}
uint32_t np_presentation_next_id(struct np_server *server)
{
    return ++server->next_presentation_id;
}
void np_presentation_time_replay_surface(struct np_surface *surface, uint32_t owner, uint32_t id)
{
    if (capture_mode) {
        assert(capture_replays < capture_member_count);
        assert(surface == capture_members[capture_replays]);
        assert(owner == capture_owner && id == capture_id);
        assert(!capture_queues && !capture_flushes);
        capture_replays++;
        return;
    }
    assert(surface == fixture_cursor && owner == surface->id && id);
    assert(!surface->pending_frame && cursor_replays == cursor_queues);
    cursor_replay_id = id;
    cursor_replays++;
}
bool np_presentation_queue_last(struct np_surface *surface, uint32_t id)
{
    if (capture_mode) {
        assert(capture_member_count == 1 && surface == capture_members[0] && id == capture_id);
        assert(capture_replays == capture_member_count && !capture_queues && !capture_flushes);
        capture_queues++;
        return capture_queue_ok;
    }
    assert(surface == fixture_cursor && id == cursor_replay_id);
    assert(cursor_replays == cursor_queues + 1);
    cursor_queues++;
    return true;
}
void np_presentation_flush(struct np_server *server)
{
    assert(capture_mode && capture_replays == capture_member_count);
    assert(capture_queues == capture_expected_queues && !capture_flushes);
    capture_flushes++;
}
void np_text_input_focus_changed(struct np_server *server)
{
    focus_resets++;
}
static bool text_field_ready;
static unsigned agent_texts;
bool np_text_input_valid_text(const char *text) { return text && !strchr(text, 0xff); }
bool np_text_input_commit_agent(struct np_server *server, struct np_surface *focus, const char *text)
{
    assert(focus && focus->window_id == server->agent_window);
    if (text_field_ready) agent_texts++;
    return text_field_ready;
}
static struct { unsigned count; uint32_t token, accepted; char problem[256]; } reply;
bool np_backend_send_binary(struct np_server *server, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    uint32_t size;
    assert(length >= 20 && !memcmp(bytes, "NPAP", 4) && bytes[5] == NP_AGENT_INPUT_REPLY);
    memcpy(&reply.token, bytes + 8, 4); memcpy(&reply.accepted, bytes + 12, 4); memcpy(&size, bytes + 16, 4);
    assert(length == 20 + size && size < sizeof(reply.problem));
    memcpy(reply.problem, bytes + 20, size); reply.problem[size] = 0;
    reply.count++;
    return true;
}
void np_backend_session_sync(struct np_server *server) {}
void np_activation_revoke_surface(struct np_surface *surface)
{
    assert(surface == fixture_surface);
    activation_resets++;
}
void np_activation_record_input(struct np_server *server, struct np_surface *surface, uint32_t serial)
{
    assert(serial && surface->window_id == server->focused_window);
    key_activations++;
}

static unsigned drain_keys(int peer, uint32_t keyboard, unsigned expected_modifiers)
{
    unsigned char bytes[2048];
    unsigned keys = 0, modifiers = 0;
    for (;;) {
        ssize_t size = recv(peer, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (size < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK); break; }
        assert(size > 0);
        for (size_t offset = 0; offset < (size_t)size;) {
            uint32_t object, header;
            assert(offset + 8 <= (size_t)size);
            memcpy(&object, bytes + offset, 4); memcpy(&header, bytes + offset + 4, 4);
            assert(object == keyboard && (header >> 16) >= 8 &&
                offset + (header >> 16) <= (size_t)size);
            const char *name = wl_keyboard_interface.events[header & 0xffff].name;
            if (!strcmp(name, "key")) keys++;
            else { assert(!strcmp(name, "modifiers")); modifiers++; }
            offset += header >> 16;
        }
    }
    assert(modifiers == expected_modifiers);
    return keys;
}

static bool dispatch_key(struct np_server *server, uint32_t window, bool pressed,
                         int malformed)
{
    struct np_window_message message;
    np_window_message_init(&message, NP_WINDOW_HOST_TO_GUEST, NP_HOST_KEY);
    np_window_put_u32(&message, window); np_window_put_u32(&message, 30);
    np_window_put_bool(&message, pressed); np_window_put_u32(&message, 1);
    assert(message.ok);
    if (malformed == 1) message.len--;  /* truncated modifier */
    if (malformed == 2) np_window_put_u8(&message, 0); /* trailing byte */
    if (malformed == 3) message.data[16] = 2; /* invalid pressed boolean */
    struct np_window_reader reader;
    assert(np_window_reader_init(&reader, message.data, message.len, NP_WINDOW_HOST_TO_GUEST));
    assert(reader.opcode == NP_HOST_KEY);
    bool accepted = handle_host_key(server, &reader);
    np_window_message_clear(&message);
    return accepted;
}

static void test_keyboard_window_isolation_and_popup_ancestors(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct np_server server = {.display = wl_display_create()};
    assert(server.display);
    wl_list_init(&server.surfaces); wl_list_init(&server.keyboards);
    struct wl_client *client = wl_client_create(server.display, sockets[0]);
    assert(client);
    struct np_surface root = {.id = 21, .window_id = 1};
    struct np_surface unrelated = {.id = 22, .window_id = 2};
    struct np_surface outer = {.id = 23, .window_id = 3, .has_grab = true, .popup_parent_window = 1};
    struct np_surface inner = {.id = 24, .window_id = 4, .has_grab = true, .popup_parent_window = 3};
    struct np_surface *surfaces[] = {&root, &unrelated, &outer, &inner};
    for (unsigned i = 0; i < 4; i++) {
        surfaces[i]->resource = wl_resource_create(client, &wl_surface_interface, 6, i + 2);
        assert(surfaces[i]->resource);
        key_surfaces[i] = surfaces[i];
        wl_list_insert(server.surfaces.next, &surfaces[i]->link);
    }
    key_surface_count = 4;
    struct np_input keyboard = {.resource = wl_resource_create(client, &wl_keyboard_interface, 9, 6)};
    assert(keyboard.resource);
    wl_list_insert(server.keyboards.next, &keyboard.link);
    key_activations = 0;

    server.focused_window = 1;
    assert(dispatch_key(&server, 1, true, 0));
    assert(drain_keys(sockets[1], 6, 1) == 1 && key_activations == 1);
    uint32_t serial = server.last_input_serial;
    for (int malformed = 1; malformed <= 3; malformed++) {
        assert(!dispatch_key(&server, 1, false, malformed));
        assert(!dispatch_key(&server, 1, true, malformed));
        assert(!drain_keys(sockets[1], 6, 0));
        assert(server.last_input_serial == serial && key_activations == 1);
    }

    /* A release queued by the old window cannot reach the new focus. Same
     * client ownership alone is insufficient: these are distinct windows. */
    server.focused_window = 2;
    assert(dispatch_key(&server, 1, false, 0));
    assert(dispatch_key(&server, 1, true, 0));
    assert(dispatch_key(&server, 0, true, 0));
    assert(!drain_keys(sockets[1], 6, 0));
    assert(server.last_input_serial == serial && key_activations == 1);
    assert(dispatch_key(&server, 2, true, 0));
    assert(drain_keys(sockets[1], 6, 1) == 1 && key_activations == 2);

    /* The innermost grabbing popup owns focus even while AppKit's key
     * toplevel or outer menu remains the source of keyboard events. */
    server.focused_window = 4;
    for (uint32_t source = 1; source <= 4; source++) {
        assert(dispatch_key(&server, source, true, 0));
        assert(drain_keys(sockets[1], 6, source == 2 ? 0 : 1) == (source == 2 ? 0 : 1));
    }
    assert(key_activations == 5);
    outer.has_grab = false;
    assert(dispatch_key(&server, 1, true, 0));
    assert(!drain_keys(sockets[1], 6, 0));
    outer.has_grab = true;
    inner.popup_parent_window = 4; /* broken/cyclic ancestry cannot grant input */
    assert(dispatch_key(&server, 1, true, 0));
    assert(!drain_keys(sockets[1], 6, 0));
    inner.popup_parent_window = 3;
    server.focused_window = 0;
    assert(dispatch_key(&server, 1, true, 0));
    assert(!drain_keys(sockets[1], 6, 0));

    wl_list_remove(&keyboard.link);
    for (unsigned i = 0; i < 4; i++) wl_list_remove(&surfaces[i]->link);
    key_surface_count = 0;
    wl_client_destroy(client); wl_display_destroy(server.display); close(sockets[1]);
}

static void test_scroll(uint32_t version, bool precise, bool inverted)
{
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	struct np_server server = {0};
	server.display = wl_display_create();
	wl_list_init(&server.pointers);
	wl_list_init(&server.keyboards);
	wl_list_init(&server.data_devices);
	wl_list_init(&server.surfaces);
	struct wl_client *client = wl_client_create(server.display, sockets[0]);
	assert(client);
	struct np_surface surface = {.id = 4, .window_id = 1};
	wl_list_insert(&server.surfaces, &surface.link);
	surface.resource = wl_resource_create(client, &wl_surface_interface, 4, 2);
	assert(surface.resource);
	fixture_surface = &surface;
	server.pointer_surface = 4;
	server.pointer_window = 1;
	struct np_input pointer = {.resource = wl_resource_create(client, &wl_pointer_interface, version, 3)};
	wl_list_insert(&server.pointers, &pointer.link);
	handle_pointer_scroll(&server, 1, 0, 0.25, precise, inverted);
	unsigned char bytes[512];
	ssize_t size = recv(sockets[1], bytes, sizeof(bytes), MSG_DONTWAIT);
	assert(size > 0);
	int discrete = 0, value120 = 0, direction = 0, axis = 0, frame = 0;
	for (size_t offset = 0; offset < (size_t)size;) {
		uint32_t object, header;
		memcpy(&object, bytes + offset, 4);
		memcpy(&header, bytes + offset + 4, 4);
		assert(object == 3 && (header >> 16) >= 8 && offset + (header >> 16) <= (size_t)size);
		const char *name = wl_pointer_interface.events[header & 0xffff].name;
		int32_t value;
		if (!strcmp(name, "axis_discrete")) {
			discrete++;
			memcpy(&value, bytes + offset + 12, 4);
			assert(value == 1);
		} else if (!strcmp(name, "axis_value120")) {
			value120++;
			memcpy(&value, bytes + offset + 12, 4);
			assert(value == 30);
		} else if (!strcmp(name, "axis_relative_direction")) {
			direction++;
			memcpy(&value, bytes + offset + 12, 4);
			assert(value == (inverted ? WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED : WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL));
		} else if (!strcmp(name, "axis")) axis++;
		else if (!strcmp(name, "frame")) frame++;
		offset += header >> 16;
	}
	assert(axis == 1 && frame == (version >= 5));
	assert(discrete == (!precise && version >= 5 && version < 8));
	assert(value120 == (!precise && version >= 8));
	assert(direction == (version >= 9));
	handle_pointer_scroll(&server, 1, NAN, 1, precise, inverted);
	assert(recv(sockets[1], bytes, sizeof(bytes), MSG_DONTWAIT) < 0);
	server.focused_window = 1;
    server.pointer_buttons = 1;
    server.pointer_grab_client = server.last_input_client = client;
    server.pointer_grab_serial = server.last_input_serial = 42;
    int previous_focus = focus_resets, previous_activation = activation_resets;
    np_input_host_disconnected(&server);
    assert(!server.focused_window && !server.pointer_window && !server.pointer_surface);
    assert(!server.pointer_buttons && !server.pointer_grab_client && !server.pointer_grab_serial);
    assert(!server.last_input_client && !server.last_input_serial && !pointer.active_scroll_axes);
    assert(focus_resets == previous_focus + 1 && activation_resets == previous_activation + 1);
    wl_list_remove(&surface.link);
	wl_list_remove(&pointer.link);
	wl_client_destroy(client);
	wl_display_destroy(server.display);
	close(sockets[1]);
	fixture_surface = NULL;
}

static void test_missing_touch(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *display = wl_display_create();
    struct wl_client *client = wl_client_create(display, sockets[0]);
    struct wl_resource *seat = wl_resource_create(client, &wl_seat_interface, 9, 2);
    assert(seat);
    seat_get_touch(client, seat, 3);
    wl_client_flush(client);
    unsigned char bytes[256];
    ssize_t size = recv(sockets[1], bytes, sizeof(bytes), MSG_DONTWAIT);
    assert(size >= 20);
    uint32_t object, bad_object, code;
    memcpy(&object, bytes, 4); memcpy(&bad_object, bytes + 8, 4); memcpy(&code, bytes + 12, 4);
    assert(object == 1 && bad_object == 2 && code == WL_SEAT_ERROR_MISSING_CAPABILITY);
    wl_client_destroy(client); wl_display_destroy(display); close(sockets[1]);
}

static void test_cursor_activation_replays_only_retained_contents(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct np_server server = {.display = wl_display_create(), .pointer_surface = 4};
    assert(server.display);
    struct wl_client *client = wl_client_create(server.display, sockets[0]);
    assert(client);
    struct np_surface root = {.id = 4, .preferred_scale = 2};
    root.resource = wl_resource_create(client, &wl_surface_interface, 1, 2);
    struct np_surface cursor = {.id = 5, .server = &server,
        .has_published = true, .last_resource_id = 42};
    cursor.resource = wl_resource_create(client, &wl_surface_interface, 1, 3);
    struct np_input pointer = {.server = &server, .last_enter_serial = 42};
    pointer.resource = wl_resource_create(client, &wl_pointer_interface, 9, 4);
    assert(root.resource && cursor.resource && pointer.resource);
    wl_resource_set_user_data(cursor.resource, &cursor);
    wl_resource_set_user_data(pointer.resource, &pointer);
    fixture_surface = &root; fixture_cursor = &cursor;
    cursor_changes = cursor_replays = cursor_queues = cursor_replay_id = 0;

    pointer_set_cursor(client, pointer.resource, 41, cursor.resource, 1, 2);
    assert(!cursor_changes && !cursor_replays && cursor.role == NP_SURFACE_ROLE_NONE);
    pointer_set_cursor(client, pointer.resource, 42, cursor.resource, 1, 2);
    assert(server.cursor_surface == cursor.resource && cursor.role == NP_SURFACE_ROLE_CURSOR);
    assert(cursor.preferred_scale == 2 && cursor_changes == 1);
    assert(cursor_replays == 1 && cursor_queues == 1 && cursor_replay_id == 1);
    unsigned char pending = 0;
    cursor.pending_frame = &pending;
    pointer_set_cursor(client, pointer.resource, 42, cursor.resource, 3, 4);
    assert(cursor_changes == 2 && cursor_replays == 1 && cursor_queues == 1);
    assert(cursor.pending_frame == &pending && server.cursor_hotspot_x == 3);
    cursor.pending_frame = NULL;
    cursor.has_published = false;
    pointer_set_cursor(client, pointer.resource, 42, cursor.resource, 0, 0);
    assert(cursor_changes == 3 && cursor_replays == 1);
    cursor.has_published = true; cursor.last_resource_id = 0;
    pointer_set_cursor(client, pointer.resource, 42, cursor.resource, 0, 0);
    assert(cursor_changes == 4 && cursor_replays == 1);
    pointer_set_cursor(client, pointer.resource, 42, NULL, 0, 0);
    assert(cursor_changes == 5 && !server.cursor_surface && cursor_replays == 1);
    cursor.role = NP_SURFACE_ROLE_DRAG_ICON;
    pointer_set_cursor(client, pointer.resource, 42, cursor.resource, 0, 0);
    assert(cursor_changes == 5 && cursor_replays == 1 && !server.cursor_surface);

    fixture_surface = fixture_cursor = NULL;
    wl_client_destroy(client); wl_display_destroy(server.display); close(sockets[1]);
}

static void expect_capture(uint32_t owner, uint32_t id, unsigned queues,
    struct np_surface *first, struct np_surface *second, struct np_surface *third)
{
    capture_mode = true;
    capture_queue_ok = true;
    capture_replays = capture_queues = capture_flushes = capture_member_count = 0;
    capture_owner = owner; capture_id = id; capture_expected_queues = queues;
    capture_members[0] = first; capture_members[1] = second; capture_members[2] = third;
    for (unsigned i = 0; i < 3; ++i) capture_member_count += capture_members[i] != NULL;
}

static void assert_no_capture(void)
{
    assert(!capture_replays && !capture_queues && !capture_flushes);
}

static void test_capture_reuses_scene_and_flat_pending_identity(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct np_server server = {.display = wl_display_create(), .next_presentation_id = 7};
    assert(server.display);
    wl_list_init(&server.surfaces);
    struct wl_client *client = wl_client_create(server.display, sockets[0]);
    assert(client);
    struct np_surface root = {.id = 101, .server = &server, .has_published = true,
        .scene_dirty = true, .scene_presentation_id = 55};
    struct np_surface child = {.id = 102, .server = &server, .parent = &root};
    struct np_surface grandchild = {.id = 103, .server = &server, .parent = &child};
    struct np_surface unrelated = {.id = 104, .server = &server, .has_published = true};
    struct np_surface cursor = {.id = 105, .server = &server, .has_published = true,
        .last_resource_id = 42, .role = NP_SURFACE_ROLE_CURSOR};
    struct np_surface *surfaces[] = {&root, &child, &grandchild, &unrelated, &cursor};
    for (unsigned i = 0; i < 5; ++i) {
        wl_list_insert(server.surfaces.prev, &surfaces[i]->link);
        surfaces[i]->resource = wl_resource_create(client, &wl_surface_interface, 1, i + 2);
        assert(surfaces[i]->resource);
    }
    fixture_cursor = &cursor;

    expect_capture(root.id, 55, 0, &root, &child, &grandchild);
    capture_current_presentation(&server, &child);
    assert(root.scene_dirty && root.scene_full_damage && root.scene_presentation_id == 55);
    assert(capture_replays == 3 && !capture_queues && capture_flushes == 1);
    assert(server.next_presentation_id == 7 && !unrelated.scene_dirty && !child.scene_dirty);
    root.scene_dirty = false; root.scene_full_damage = false;
    expect_capture(root.id, 8, 0, &root, &child, &grandchild);
    capture_current_presentation(&server, &root);
    assert(root.scene_dirty && root.scene_full_damage && root.scene_presentation_id == 8);
    assert(server.next_presentation_id == 8 && capture_replays == 3 && capture_flushes == 1);
    root.has_published = false; root.scene_dirty = root.scene_full_damage = false;
    expect_capture(0, 0, 0, NULL, NULL, NULL);
    capture_current_presentation(&server, &root);
    capture_current_presentation(&server, NULL);
    assert_no_capture();
    assert(!root.scene_dirty && !root.scene_full_damage && root.scene_presentation_id == 8);

    server.cursor_surface = cursor.resource;
    expect_capture(cursor.id, 9, 1, &cursor, NULL, NULL);
    capture_current_presentation(&server, &cursor);
    assert(capture_replays == 1 && capture_queues == 1 && capture_flushes == 1);
    assert(server.next_presentation_id == 9);
    cursor.role = NP_SURFACE_ROLE_DRAG_ICON;
    server.cursor_surface = NULL; server.drag_icon = cursor.resource;
    expect_capture(cursor.id, 10, 1, &cursor, NULL, NULL);
    capture_current_presentation(&server, &cursor);
    assert(capture_replays == 1 && capture_queues == 1 && capture_flushes == 1);
    assert(server.next_presentation_id == 10);
    expect_capture(cursor.id, 11, 1, &cursor, NULL, NULL);
    capture_queue_ok = false;
    capture_current_presentation(&server, &cursor);
    assert(capture_replays == 1 && capture_queues == 1 && !capture_flushes);
    assert(!cursor.pending_frame && server.next_presentation_id == 11);
    unsigned char pending = 0;
    cursor.pending_frame = &pending; cursor.pending_frame_size = 1; cursor.pending_presentation_id = 77;
    expect_capture(cursor.id, 77, 0, &cursor, NULL, NULL);
    capture_current_presentation(&server, &cursor);
    assert(capture_replays == 1 && !capture_queues && capture_flushes == 1);
    assert(cursor.pending_frame == &pending && cursor.pending_presentation_id == 77);
    assert(cursor.pending_frame_size == 1 && server.next_presentation_id == 11);
    cursor.role = NP_SURFACE_ROLE_CURSOR;
    server.drag_icon = NULL; server.cursor_surface = cursor.resource;
    expect_capture(cursor.id, 77, 0, &cursor, NULL, NULL);
    capture_current_presentation(&server, &cursor);
    assert(capture_replays == 1 && !capture_queues && capture_flushes == 1);
    assert(cursor.pending_frame == &pending && cursor.pending_presentation_id == 77);
    server.cursor_surface = NULL;
    expect_capture(0, 0, 0, NULL, NULL, NULL);
    capture_current_presentation(&server, &cursor); /* Nonactive cursor. */
    cursor.role = NP_SURFACE_ROLE_DRAG_ICON;
    capture_current_presentation(&server, &cursor); /* Nonactive icon. */
    cursor.role = NP_SURFACE_ROLE_CURSOR;
    server.cursor_surface = cursor.resource; cursor.has_published = false;
    capture_current_presentation(&server, &cursor);
    cursor.has_published = true; cursor.last_resource_id = 0;
    capture_current_presentation(&server, &cursor);
    cursor.last_resource_id = 42; cursor.role = NP_SURFACE_ROLE_NONE;
    capture_current_presentation(&server, &cursor);
    assert_no_capture();
    assert(cursor.pending_frame == &pending && cursor.pending_presentation_id == 77);
    assert(server.next_presentation_id == 11);

    capture_mode = false; fixture_cursor = NULL;
    wl_client_destroy(client); wl_display_destroy(server.display); close(sockets[1]);
}

bool np_trace_enabled(void) { return false; }


/* Counts one client's wl_keyboard events by name. */
struct keyboard_events { unsigned enter, leave, key, modifiers; uint32_t entered; };
static struct keyboard_events keyboard_events(struct np_server *server, int peer, uint32_t keyboard)
{
    wl_display_flush_clients(server->display);
    struct keyboard_events events = {0};
    unsigned char bytes[4096];
    for (;;) {
        ssize_t size = recv(peer, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (size < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK); break; }
        assert(size > 0);
        for (size_t offset = 0; offset < (size_t)size;) {
            uint32_t object, header;
            memcpy(&object, bytes + offset, 4); memcpy(&header, bytes + offset + 4, 4);
            assert(object == keyboard && (header >> 16) >= 8 && offset + (header >> 16) <= (size_t)size);
            const char *name = wl_keyboard_interface.events[header & 0xffff].name;
            if (!strcmp(name, "enter")) { events.enter++; memcpy(&events.entered, bytes + offset + 12, 4); }
            else if (!strcmp(name, "leave")) events.leave++;
            else if (!strcmp(name, "key")) events.key++;
            else { assert(!strcmp(name, "modifiers")); events.modifiers++; }
            offset += header >> 16;
        }
    }
    return events;
}

enum { AGENT_KEY = 1, AGENT_TEXT = 2, AGENT_BAD = 3 };
static bool agent_request(struct np_server *server, uint32_t token, uint32_t window,
                          const int *kinds, unsigned count)
{
    struct np_window_message message;
    np_window_message_init(&message, NP_WINDOW_HOST_TO_GUEST, NP_HOST_AGENT_INPUT);
    np_window_put_u32(&message, token); np_window_put_u32(&message, window);
    np_window_put_u32(&message, count);
    for (unsigned i = 0; i < count; i++) {
        np_window_put_u8(&message, (uint8_t)kinds[i]);
        if (kinds[i] == AGENT_TEXT) np_window_put_string(&message, "你好");
        else { np_window_put_u32(&message, 30); np_window_put_bool(&message, i % 2 == 0); np_window_put_u32(&message, 1); }
    }
    assert(message.ok);
    struct np_window_reader reader;
    assert(np_window_reader_init(&reader, message.data, message.len, NP_WINDOW_HOST_TO_GUEST));
    bool accepted = handle_agent_input(server, &reader);
    np_window_message_clear(&message);
    return accepted;
}

static void test_agent_focus_alongside_user(void)
{
    int user_pair[2], agent_pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, user_pair) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, agent_pair) == 0);
    struct np_server server = {.display = wl_display_create()};
    assert(server.display);
    wl_list_init(&server.surfaces); wl_list_init(&server.keyboards);
    wl_list_init(&server.pointers); wl_list_init(&server.data_devices);
    struct wl_client *editor = wl_client_create(server.display, user_pair[0]);
    struct wl_client *terminal = wl_client_create(server.display, agent_pair[0]);
    assert(editor && terminal);
    struct np_surface notes = {.id = 31, .window_id = 1}, other_notes = {.id = 32, .window_id = 2};
    struct np_surface shell = {.id = 33, .window_id = 3};
    struct np_surface menu = {.id = 34, .window_id = 4, .popup_parent_window = 3};
    struct np_surface *surfaces[] = {&notes, &other_notes, &shell, &menu};
    for (unsigned i = 0; i < 4; i++) {
        surfaces[i]->resource = wl_resource_create(i < 2 ? editor : terminal, &wl_surface_interface, 6, 2 + i % 2);
        assert(surfaces[i]->resource);
        key_surfaces[i] = surfaces[i];
        wl_list_insert(server.surfaces.next, &surfaces[i]->link);
    }
    key_surface_count = 4;
    struct np_input editor_keyboard = {.resource = wl_resource_create(editor, &wl_keyboard_interface, 9, 4)};
    struct np_input terminal_keyboard = {.resource = wl_resource_create(terminal, &wl_keyboard_interface, 9, 4)};
    assert(editor_keyboard.resource && terminal_keyboard.resource);
    wl_list_insert(&server.keyboards, &editor_keyboard.link);
    wl_list_insert(&server.keyboards, &terminal_keyboard.link);
    key_activations = 0; reply.count = 0; text_field_ready = false; agent_texts = 0;
    const int keys[] = {AGENT_KEY, AGENT_KEY};

    np_set_keyboard_focus(&server, 1);
    struct keyboard_events e = keyboard_events(&server, user_pair[1], 4);
    assert(e.enter == 1 && e.entered == 2);

    /* The agent types into another client without disturbing the user's. */
    assert(agent_request(&server, 7, 3, keys, 2));
    assert(reply.count == 1 && reply.token == 7 && reply.accepted == 2 && !reply.problem[0]);
    assert(server.focused_window == 1 && server.agent_window == 3 && !key_activations);
    e = keyboard_events(&server, agent_pair[1], 4);
    assert(e.enter == 1 && e.entered == 2 && e.key == 2 && e.modifiers == 2);
    e = keyboard_events(&server, user_pair[1], 4);
    assert(!e.enter && !e.leave && !e.key);
    /* The user keeps typing in their own window meanwhile. */
    assert(dispatch_key(&server, 1, true, 0));
    e = keyboard_events(&server, user_pair[1], 4);
    assert(e.key == 1 && key_activations == 1);
    assert(!keyboard_events(&server, agent_pair[1], 4).key);

    /* One client has one keyboard focus: the user's window can be shared,
     * but another window of the user's client is refused. */
    assert(agent_request(&server, 8, 2, keys, 2));
    assert(reply.count == 2 && reply.accepted == 0 && strstr(reply.problem, "user is typing"));
    assert(!keyboard_events(&server, user_pair[1], 4).key && server.agent_window == 3);
    assert(agent_request(&server, 9, 1, keys, 2));
    assert(reply.count == 3 && reply.accepted == 2 && server.agent_window == 3);
    assert(keyboard_events(&server, user_pair[1], 4).key == 2);

    /* A grab opened by agent input takes the agent's focus, and gives it back. */
    menu.has_grab = true;
    np_keyboard_focus_grab(&server, &menu);
    assert(menu.grab_agent && server.agent_window == 4 && server.focused_window == 1);
    e = keyboard_events(&server, agent_pair[1], 4);
    assert(e.leave == 1 && e.enter == 1 && e.entered == 3);
    assert(agent_request(&server, 10, 3, keys, 2));
    assert(reply.accepted == 2 && keyboard_events(&server, agent_pair[1], 4).key == 2);
    /* The user's focus changes are not redirected into the agent's menu. */
    handle_keyboard_focus(&server, 2);
    assert(server.focused_window == 2 && server.agent_window == 4);
    handle_keyboard_focus(&server, 1);
    assert(server.focused_window == 1 && server.agent_window == 4);
    menu.has_grab = false;
    np_keyboard_focus_ungrab(&server, &menu);
    assert(server.agent_window == 3 && !menu.grab_agent);
    e = keyboard_events(&server, agent_pair[1], 4);
    assert(e.leave == 1 && e.enter == 1 && e.entered == 2);
    keyboard_events(&server, user_pair[1], 4);

    /* Text waits for the client to enable a field, then reports the gap. */
    const int text[] = {AGENT_KEY, AGENT_TEXT, AGENT_KEY};
    assert(agent_request(&server, 11, 3, text, 3));
    assert(reply.count == 4 && server.agent_input && server.agent_input->next == 1);
    assert(agent_request(&server, 12, 3, keys, 2));
    assert(reply.count == 5 && reply.token == 12 && strstr(reply.problem, "still running"));
    text_field_ready = true;
    agent_input_resume(&server);
    assert(reply.count == 6 && reply.token == 11 && reply.accepted == 3 && agent_texts == 1 && !server.agent_input);
    text_field_ready = false;
    assert(agent_request(&server, 13, 3, text, 3));
    server.agent_input->wait_until_ms = 1;
    agent_input_resume(&server);
    assert(reply.count == 7 && reply.token == 13 && reply.accepted == 1 && strstr(reply.problem, "no text-input"));
    keyboard_events(&server, agent_pair[1], 4);

    /* Malformed requests fail as a whole, before any event is delivered. */
    const int bad[] = {AGENT_KEY, AGENT_BAD};
    assert(!agent_request(&server, 14, 3, bad, 2));
    assert(!agent_request(&server, 0, 3, keys, 2));
    assert(reply.count == 7 && !keyboard_events(&server, agent_pair[1], 4).key);
    assert(agent_request(&server, 15, 9, keys, 2));
    assert(reply.count == 8 && reply.accepted == 0 && strstr(reply.problem, "window closed"));

    /* When the user moves into the agent's window the two share it, and
     * moving away again leaves the agent's focus where it was. */
    np_set_keyboard_focus(&server, 3);
    assert(!keyboard_events(&server, agent_pair[1], 4).enter && server.agent_window == 3);
    assert(keyboard_events(&server, user_pair[1], 4).leave == 1);
    np_set_keyboard_focus(&server, 1);
    e = keyboard_events(&server, agent_pair[1], 4);
    assert(!e.enter && !e.leave && server.agent_window == 3);
    /* The user taking another window of the agent's client wins it. */
    struct np_surface second_shell = {.id = 35, .window_id = 5};
    second_shell.resource = wl_resource_create(terminal, &wl_surface_interface, 6, 5);
    key_surfaces[4] = &second_shell; key_surface_count = 5;
    keyboard_events(&server, user_pair[1], 4);
    np_set_keyboard_focus(&server, 5);
    assert(server.focused_window == 5 && !server.agent_window);
    e = keyboard_events(&server, agent_pair[1], 4);
    assert(e.leave == 1 && e.enter == 1 && e.entered == 5);

    /* Activation revocation on disconnect has its own fixture. */
    for (unsigned i = 0; i < 4; i++) wl_list_remove(&surfaces[i]->link);
    np_input_host_disconnected(&server);
    assert(!server.focused_window && !server.agent_window && !server.agent_input);
    assert(keyboard_events(&server, agent_pair[1], 4).leave == 1);

    wl_list_remove(&editor_keyboard.link); wl_list_remove(&terminal_keyboard.link);
    key_surface_count = 0;
    wl_client_destroy(editor); wl_client_destroy(terminal); wl_display_destroy(server.display);
    close(user_pair[1]); close(agent_pair[1]);
}

int main(void)
{
	for (uint32_t version = 4; version <= 9; version++) {
		test_scroll(version, false, false);
		test_scroll(version, false, true);
		test_scroll(version, true, true);
	}
	test_missing_touch();
	test_cursor_activation_replays_only_retained_contents();
	test_capture_reuses_scene_and_flat_pending_identity();
	test_keyboard_window_isolation_and_popup_ancestors();
	test_agent_focus_alongside_user();
	assert(wheel_value120(1e300) == INT32_MAX);
	assert(wheel_value120(-1e300) == INT32_MIN);
	puts("PASS: pointer v4-v9 scroll, disconnect, cursor activation, capture replay and keyboard window isolation, agent focus");
	return 0;
}
