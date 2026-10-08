/* Real libwayland resources and pipes; only scene hit testing / host output
 * are fixtures. Exercise the same handlers used by wl_data_offer requests. */
#include "../data_device.c"
#include <assert.h>
#include <signal.h>
#include <sys/socket.h>

static struct np_surface surface;
static unsigned restored, last_action, last_token;
static bool last_success;
static struct np_surface *fixture_icon;
static unsigned icon_changes, icon_replays, icon_queues;
static uint32_t icon_replay_id;
struct np_surface *np_surface_by_window(struct np_server *s, uint32_t id) { return id == 1 ? &surface : NULL; }
struct np_surface *np_surface_by_id(struct np_server *s, uint32_t id) { return id == 1 ? &surface : NULL; }
struct np_surface *np_scene_root(struct np_surface *s) { return s; }
struct np_surface *np_scene_hit_test(struct np_surface *s, double x, double y, double *lx, double *ly) {
    *lx = x; *ly = y; return s;
}
bool np_surface_assign_role(struct np_surface *s, enum np_surface_role role) {
    if (s->role != NP_SURFACE_ROLE_NONE && s->role != role) return false;
    s->role = role;
    return true;
}
void np_scale_changed(struct np_surface *s, int scale) { s->preferred_scale = scale; }
void np_input_clear_pointer_focus_for_drag(struct np_server *s) {}
void np_input_restore_pointer_focus_after_drag(struct np_server *s, struct np_surface *target) { restored++; }
bool np_window_event_send(struct np_server *s, uint8_t op, const uint32_t *v, size_t count) {
    if (fixture_icon) {
        assert(op == NP_GUEST_DRAG_ICON_CHANGED && count == 1);
        assert(v[0] == fixture_icon->id || v[0] == 0);
        icon_changes++;
    }
    return true;
}
uint32_t np_presentation_next_id(struct np_server *s) { return ++s->next_presentation_id; }
void np_presentation_time_replay_surface(struct np_surface *s, uint32_t owner, uint32_t id) {
    assert(s == fixture_icon && owner == s->id && id);
    assert(!s->pending_frame && icon_replays == icon_queues);
    icon_replay_id = id;
    icon_replays++;
}
bool np_presentation_queue_last(struct np_surface *s, uint32_t id) {
    assert(s == fixture_icon && id == icon_replay_id && icon_replays == icon_queues + 1);
    icon_queues++;
    return true;
}
bool np_window_event_send_message(struct np_server *s, struct np_window_message *m) {
    if (m->data[5] != NP_GUEST_FILE_DRAG) return true;
    struct np_window_reader r;
    assert(np_window_reader_init(&r, m->data, m->len, NP_WINDOW_GUEST_TO_HOST));
    last_action = np_window_read_u32(&r); last_token = np_window_read_u32(&r);
    np_window_read_u32(&r); np_window_read_f64(&r); np_window_read_f64(&r);
    const unsigned char *bytes; size_t size; bool present;
    assert(np_window_read_bytes(&r, &bytes, &size, true, &present));
    last_success = present && size == 1 && bytes[0];
    return true;
}

static void command(struct np_server *s, unsigned action, unsigned token, const char *payload) {
    struct np_window_message m;
    np_window_message_init(&m, NP_WINDOW_HOST_TO_GUEST, NP_HOST_FILE_DRAG);
    np_window_put_u32(&m, action); np_window_put_u32(&m, token); np_window_put_u32(&m, 1);
    np_window_put_f64(&m, 10); np_window_put_f64(&m, 20);
    np_window_put_optional_bytes(&m, (const unsigned char *)payload, payload ? strlen(payload) : 0, payload != NULL);
    struct np_window_reader r;
    assert(np_window_reader_init(&r, m.data, m.len, NP_WINDOW_HOST_TO_GUEST));
    assert(np_data_handle_file_drag(s, &r));
    np_window_message_clear(&m);
}
static struct np_data_offer *enter(struct np_server *s, struct wl_client *client, unsigned token) {
    command(s, 1, token, NULL);
    struct np_data_offer *offer = wl_container_of(s->data_offers.next, offer, link);
    assert(offer->host_drag == token && offer->active);
    data_offer_accept(client, offer->resource, 0, "text/plain");
    assert(!offer->accepted_mime);
    data_offer_accept(client, offer->resource, 0, "text/uri-list");
    data_offer_set_actions(client, offer->resource, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                           WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    assert(last_action == 103 && last_token == token && last_success);
    return offer;
}
static int receive_type(struct np_data_offer *offer, struct wl_client *client, const char *mime) {
    int p[2]; assert(pipe(p) == 0);
    data_offer_receive(client, offer->resource, mime, p[1]);
    return p[0];
}
static int receive(struct np_data_offer *offer, struct wl_client *client) {
    return receive_type(offer, client, "text/uri-list");
}

struct drag_events { unsigned cancelled, finished, leave, send; int fd; };
static struct drag_events drain_drag_events(struct np_server *s, int peer,
                                             struct wl_resource *source, struct wl_resource *device) {
    struct drag_events result = {.fd = -1};
    wl_display_flush_clients(s->display);
    for (;;) {
        unsigned char bytes[8192];
        union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
        struct iovec io = {.iov_base = bytes, .iov_len = sizeof(bytes)};
        struct msghdr message = {.msg_iov = &io, .msg_iovlen = 1,
            .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
        ssize_t size = recvmsg(peer, &message, MSG_DONTWAIT);
        if (size < 0 && errno == EINTR) continue;
        if (size < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK); break; }
        if (!size) break;
        assert(!(message.msg_flags & MSG_CTRUNC));
        for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
            assert(header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS);
            assert(header->cmsg_len == CMSG_LEN(sizeof(int)) && result.fd < 0);
            memcpy(&result.fd, CMSG_DATA(header), sizeof(result.fd));
        }
        for (size_t offset = 0; offset < (size_t)size;) {
            uint32_t object, header;
            assert(offset + 8 <= (size_t)size);
            memcpy(&object, bytes + offset, 4); memcpy(&header, bytes + offset + 4, 4);
            size_t length = header >> 16;
            assert(length >= 8 && offset + length <= (size_t)size);
            uint16_t opcode = (uint16_t)header;
            if (object == wl_resource_get_id(source)) {
                result.cancelled += opcode == WL_DATA_SOURCE_CANCELLED;
                result.finished += opcode == WL_DATA_SOURCE_DND_FINISHED;
                result.send += opcode == WL_DATA_SOURCE_SEND;
            }
            if (object == wl_resource_get_id(device)) result.leave += opcode == WL_DATA_DEVICE_LEAVE;
            offset += length;
        }
    }
    return result;
}
static struct np_data_source *guest_source(struct np_server *s, struct wl_client *client) {
    struct np_data_source *source = calloc(1, sizeof(*source)); assert(source);
    source->server = s; source->actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    source->resource = wl_resource_create(client, &wl_data_source_interface, 3, 0); assert(source->resource);
    wl_resource_set_implementation(source->resource, &data_source_implementation, source, data_source_resource_destroy);
    data_source_offer(client, source->resource, "text/plain");
    return source;
}
static struct np_data_offer *guest_drag(struct np_server *s, struct wl_client *client,
                                        struct wl_resource *device, struct np_data_source *source, uint32_t serial) {
    s->pointer_window = s->pointer_surface = 1; s->pointer_buttons = 1;
    s->pointer_grab_client = client; s->pointer_grab_serial = serial;
    data_device_start_drag(client, device, source->resource, surface.resource, NULL, serial);
    assert(s->drag_source == source->resource && !s->drag_exported && !s->drag_dropped);
    struct np_data_offer *offer = wl_container_of(s->data_offers.next, offer, link);
    assert(offer->active && offer->source == source->resource);
    data_offer_accept(client, offer->resource, serial, "text/plain");
    data_offer_set_actions(client, offer->resource, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                           WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    return offer;
}

static void test_guest_disconnect(struct np_server *s, struct wl_client *client,
                                    struct wl_resource *device, int peer) {
    struct np_data_source *source = guest_source(s, client);
    struct np_data_offer *offer = guest_drag(s, client, device, source, 91);
    struct drag_events events = drain_drag_events(s, peer, source->resource, device);
    assert(!events.cancelled && events.fd < 0);
    np_data_host_disconnected(s);
    assert(!s->drag_source && !s->drag_origin && !s->drag_icon && !s->drag_focus_window && !s->drag_focus_surface);
    assert(!s->drag_dropped && !s->drag_exported && !s->pointer_buttons);
    assert(!offer->source && !offer->active);
    events = drain_drag_events(s, peer, source->resource, device);
    assert(events.cancelled == 1 && events.leave == 1 && !events.finished && events.fd < 0);
    int fd = receive_type(offer, client, "text/plain"); char bytes[64];
    assert(read(fd, bytes, sizeof(bytes)) == 0); close(fd);
    data_offer_finish(client, offer->resource);
    np_data_host_disconnected(s); /* Cancellation is idempotent. */
    events = drain_drag_events(s, peer, source->resource, device);
    assert(!events.cancelled && !events.finished && !events.send && events.fd < 0);
    wl_resource_destroy(offer->resource); wl_resource_destroy(source->resource);

    source = guest_source(s, client);
    offer = guest_drag(s, client, device, source, 92);
    np_data_drag_finish(s);
    assert(s->drag_dropped && offer->dropped && offer->source == source->resource);
    events = drain_drag_events(s, peer, source->resource, device);
    assert(!events.cancelled && !events.finished && events.fd < 0);
    fd = receive_type(offer, client, "text/plain"); /* A guest-only pipe is already requested. */
    np_data_host_disconnected(s);
    assert(s->drag_source == source->resource && s->drag_dropped && !s->drag_exported);
    assert(offer->source == source->resource && offer->dropped && !offer->finished);
    events = drain_drag_events(s, peer, source->resource, device);
    assert(!events.cancelled && !events.finished && !events.leave && events.send == 1 && events.fd >= 0);
    const char *payload = "guest transfer survives host disconnect";
    assert(write(events.fd, payload, strlen(payload)) == (ssize_t)strlen(payload)); close(events.fd);
    assert(read(fd, bytes, sizeof(bytes)) == (ssize_t)strlen(payload));
    assert(!memcmp(bytes, payload, strlen(payload)) && read(fd, bytes, sizeof(bytes)) == 0); close(fd);
    fd = receive_type(offer, client, "text/plain"); /* The dropped offer also remains usable. */
    events = drain_drag_events(s, peer, source->resource, device);
    assert(events.send == 1 && events.fd >= 0);
    assert(write(events.fd, "again", 5) == 5); close(events.fd);
    assert(read(fd, bytes, sizeof(bytes)) == 5 && !memcmp(bytes, "again", 5)); close(fd);
    data_offer_finish(client, offer->resource);
    events = drain_drag_events(s, peer, source->resource, device);
    assert(events.finished == 1 && !events.cancelled && !s->drag_source && events.fd < 0);
    wl_resource_destroy(offer->resource); wl_resource_destroy(source->resource);
}

static void test_icon_activation_replays_only_retained_contents(struct np_server *s,
    struct wl_client *client, struct wl_resource *device)
{
    struct np_data_source *source = guest_source(s, client);
    struct np_surface icon = {.id = 2, .server = s,
        .has_published = true, .last_resource_id = 42};
    icon.resource = wl_resource_create(client, &wl_surface_interface, 1, 0);
    assert(icon.resource);
    wl_resource_set_user_data(icon.resource, &icon);
    fixture_icon = &icon;
    icon_changes = icon_replays = icon_queues = icon_replay_id = 0;
    surface.preferred_scale = 2;
    s->pointer_surface = surface.id; s->pointer_window = surface.window_id;
    s->pointer_buttons = 1; s->pointer_grab_client = client; s->pointer_grab_serial = 99;
    data_device_start_drag(client, device, source->resource, surface.resource, icon.resource, 98);
    assert(!icon_changes && !icon_replays && icon.role == NP_SURFACE_ROLE_NONE);
    data_device_start_drag(client, device, source->resource, surface.resource, icon.resource, 99);
    assert(s->drag_icon == icon.resource && icon.role == NP_SURFACE_ROLE_DRAG_ICON);
    assert(icon.preferred_scale == 2 && icon_changes == 1 && icon_replays == 1 && icon_queues == 1);
    unsigned char pending = 0;
    icon.pending_frame = &pending;
    data_device_start_drag(client, device, source->resource, surface.resource, icon.resource, 99);
    assert(icon_changes == 2 && icon_replays == 1 && icon.pending_frame == &pending);
    icon.pending_frame = NULL; icon.has_published = false;
    data_device_start_drag(client, device, source->resource, surface.resource, icon.resource, 99);
    assert(icon_changes == 3 && icon_replays == 1);
    icon.has_published = true; icon.last_resource_id = 0;
    data_device_start_drag(client, device, source->resource, surface.resource, icon.resource, 99);
    assert(icon_changes == 4 && icon_replays == 1);
    data_device_start_drag(client, device, source->resource, surface.resource, NULL, 99);
    assert(icon_changes == 5 && icon_replays == 1 && !s->drag_icon);
    icon.role = NP_SURFACE_ROLE_CURSOR;
    data_device_start_drag(client, device, source->resource, surface.resource, icon.resource, 99);
    assert(icon_changes == 5 && icon_replays == 1 && !s->drag_icon);
    fixture_icon = NULL;
    wl_resource_destroy(source->resource);
    wl_resource_destroy(icon.resource);
}
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    struct np_server s = {0};
    s.display = wl_display_create(); assert(s.display);
    wl_list_init(&s.data_devices); wl_list_init(&s.data_offers);
    wl_list_init(&s.clip_pending); wl_list_init(&s.clip_reads); wl_list_init(&s.clip_writes);
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_client *client = wl_client_create(s.display, sockets[0]); assert(client);
    /* Releasing the v4 factory leaves its existing data source and device
     * alive. A subsequent offer still uses the ordinary clipboard/DnD path. */
    struct wl_resource *manager = wl_resource_create(client, &wl_data_device_manager_interface, 4, 0);
    assert(manager); wl_resource_set_user_data(manager, &s);
    data_manager_create_source(client, manager, 2);
    data_manager_get_device(client, manager, 3, NULL);
    struct wl_resource *owned_source = wl_client_get_object(client, 2);
    struct wl_resource *owned_device = wl_client_get_object(client, 3);
    assert(owned_source && owned_device);
    assert(data_manager_implementation.release);
    data_manager_implementation.release(client, manager);
    assert(wl_client_get_object(client, 2) == owned_source);
    assert(wl_client_get_object(client, 3) == owned_device);
    data_source_offer(client, owned_source, "text/plain");
    assert(((struct np_data_source *)wl_resource_get_user_data(owned_source))->mime_count == 1);
    wl_resource_destroy(owned_device); wl_resource_destroy(owned_source);
    surface = (struct np_surface){ .id = 1, .window_id = 1, .server = &s };
    surface.resource = wl_resource_create(client, &wl_surface_interface, 6, 0);
    wl_resource_set_user_data(surface.resource, &surface);
    struct np_input *device = calloc(1, sizeof(*device)); assert(device);
    device->server = &s;
    device->resource = wl_resource_create(client, &wl_data_device_interface, 3, 0);
    wl_resource_set_implementation(device->resource, &data_device_implementation, device, data_device_resource_destroy);
    wl_list_insert(&s.data_devices, &device->link);

    struct np_data_offer *first = enter(&s, client, 1);
    int fd = receive(first, client);
    assert(last_action == 105 && last_token == 1);
    command(&s, 4, 1, NULL);
    assert(first->dropped && restored == 1 && !s.host_file_drag);
    struct np_data_offer *second = enter(&s, client, 2);
    /* A second drag must not invalidate the previous dropped offer's read. */
    const char *uri = "file:///tmp/first.txt\r\n";
    command(&s, 5, 1, uri);
    char bytes[100]; assert(read(fd, bytes, sizeof(bytes)) == (ssize_t)strlen(uri));
    assert(!memcmp(bytes, uri, strlen(uri))); assert(read(fd, bytes, sizeof(bytes)) == 0); close(fd);
    data_offer_finish(client, first->resource);
    assert(last_action == 104 && last_token == 1 && last_success);
    wl_resource_destroy(first->resource);

    fd = receive(second, client);
    command(&s, 3, 2, NULL);
    assert(read(fd, bytes, sizeof(bytes)) == 0); close(fd);
    fd = receive(second, client); /* A stale offer never opens another read. */
    assert(read(fd, bytes, sizeof(bytes)) == 0); close(fd);
    wl_resource_destroy(second->resource);
    first = enter(&s, client, 3);
    command(&s, 4, 3, NULL);
    wl_resource_destroy(first->resource);
    assert(last_action == 104 && last_token == 3 && !last_success);

    first = enter(&s, client, 4); fd = receive(first, client);
    np_data_host_disconnected(&s);
    assert(read(fd, bytes, sizeof(bytes)) == 0); close(fd);
    assert(!s.host_file_drag && wl_list_empty(&s.clip_pending));

    struct np_data_source *source = guest_source(&s, client);
    s.drag_source = source->resource; s.drag_origin = surface.resource;
    s.file_drag_serial = 20; s.pointer_buttons = 1;
    command(&s, 7, 20, NULL);
    assert(s.drag_exported);
    np_data_drag_finish(&s); /* AppKit's drag, not a stale normal mouse-up, owns completion. */
    assert(s.drag_source == source->resource && !s.drag_dropped);
    command(&s, 9, 20, NULL); assert(s.drag_dropped);
    command(&s, 8, 19, "\1"); assert(s.drag_source == source->resource);
    command(&s, 8, 20, "\1");
    assert(!s.drag_source && !s.drag_exported && !s.pointer_buttons);
    /* Disconnect also cancels an exported source if AppKit never sends end. */
    s.drag_source = source->resource; s.drag_exported = true; s.pointer_buttons = 1;
    np_data_host_disconnected(&s);
    assert(!s.drag_source && !s.drag_exported && !s.pointer_buttons);
    test_guest_disconnect(&s, client, device->resource, sockets[1]);
    test_icon_activation_replays_only_retained_contents(&s, client, device->resource);
    wl_client_destroy(client); close(sockets[1]); wl_display_destroy(s.display);
    puts("file DnD: factory release, transfers, finish/cancel/disconnect and retained icon activation PASS");
}
