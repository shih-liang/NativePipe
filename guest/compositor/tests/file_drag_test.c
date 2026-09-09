/* Real libwayland resources and pipes; only scene hit testing / host output
 * are fixtures. Exercise the same handlers used by wl_data_offer requests. */
#include "../data_device.c"
#include <assert.h>
#include <signal.h>
#include <sys/socket.h>

static struct np_surface surface;
static unsigned restored, last_action, last_token;
static bool last_success;
struct np_surface *np_surface_by_window(struct np_server *s, uint32_t id) { return id == 1 ? &surface : NULL; }
struct np_surface *np_surface_by_id(struct np_server *s, uint32_t id) { return id == 1 ? &surface : NULL; }
struct np_surface *np_scene_root(struct np_surface *s) { return s; }
struct np_surface *np_scene_hit_test(struct np_surface *s, double x, double y, double *lx, double *ly) {
    *lx = x; *ly = y; return s;
}
bool np_surface_assign_role(struct np_surface *s, enum np_surface_role role) { return true; }
void np_scale_changed(struct np_surface *s, int scale) {}
void np_input_clear_pointer_focus_for_drag(struct np_server *s) {}
void np_input_restore_pointer_focus_after_drag(struct np_server *s, struct np_surface *target) { restored++; }
bool np_window_event_send(struct np_server *s, uint8_t op, const uint32_t *v, size_t count) { return true; }
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
static int receive(struct np_data_offer *offer, struct wl_client *client) {
    int p[2]; assert(pipe(p) == 0);
    data_offer_receive(client, offer->resource, "text/uri-list", p[1]);
    return p[0];
}
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    struct np_server s = {0};
    s.display = wl_display_create(); assert(s.display);
    wl_list_init(&s.data_devices); wl_list_init(&s.data_offers);
    wl_list_init(&s.clip_pending); wl_list_init(&s.clip_reads); wl_list_init(&s.clip_writes);
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_client *client = wl_client_create(s.display, sockets[0]); assert(client);
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

    struct np_data_source *source = calloc(1, sizeof(*source)); assert(source);
    source->server = &s; source->actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    source->resource = wl_resource_create(client, &wl_data_source_interface, 3, 0);
    wl_resource_set_implementation(source->resource, &data_source_implementation, source, data_source_resource_destroy);
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
    wl_client_destroy(client); close(sockets[1]); wl_display_destroy(s.display);
    puts("file DnD: negotiation, overlapping transfers, finish, cancel, EOF, export and disconnect PASS");
}
