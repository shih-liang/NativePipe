/* Exercise real wl_surface/xdg-shell resources and their serialized events.
 * Only backend ownership and output geometry are fixtures; version gates,
 * configure serials, metadata ordering and the latest resize mailbox are real. */
#include "../surface.c"
#include "../xdg_shell.c"
#include "../region.c"

#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

bool np_trace_enabled(void) { return false; }
void np_surface_commit(struct wl_client *client, struct wl_resource *resource) {}
void np_surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {}
void np_surface_update_destroy(struct np_surface_update *update, bool release_buffer) {}
void np_presentation_clear_scene_wait(struct np_surface *surface) {}
void np_presentation_time_discard_surface(struct np_surface *surface) {}
void np_activation_revoke_surface(struct np_surface *surface) {}
uint32_t np_presentation_next_id(struct np_server *server) { return 1; }
void np_presentation_queue_scene(struct np_surface *surface, uint32_t id) {}
void np_presentation_set_current_buffer(struct np_surface *surface,
    struct wl_resource *buffer, struct np_gpu_buffer *gpu, struct np_sync_point *point) {}
void np_syncobj_surface_destroyed(struct np_surface *surface) {}
void np_backend_surface_destroy(struct np_surface *surface) {}
void np_subsurface_detach_tree(struct np_surface *surface) {}
struct np_surface *np_scene_root(struct np_surface *surface) { return surface; }
void np_scene_destroy(struct np_surface *surface) {}
void np_scale_surface_enter_outputs(struct np_surface *surface, struct wl_client *client) {}
void np_scale_surface_bounds(const struct np_surface *surface, int32_t *width, int32_t *height) {
    *width = surface->server->output_width / surface->server->output_scale;
    *height = surface->server->output_height / surface->server->output_scale;
}
void np_decoration_use_client_default(struct np_surface *surface) {}
bool np_xwayland_owns_client(struct np_server *server, struct wl_client *client) { return false; }
void np_set_keyboard_focus(struct np_server *server, uint32_t window) {}
bool np_window_event_send(struct np_server *server, uint8_t op,
                           const uint32_t *values, size_t count) { return true; }
bool np_window_event_send_message(struct np_server *server,
                                   struct np_window_message *message) { return true; }
bool np_window_event_send_force_quit_capability(struct np_server *server,
                                                uint32_t window, bool supported) { return true; }
bool np_window_event_send_popup_placement(struct np_server *server,
    uint32_t window, uint32_t parent, int32_t x, int32_t y, int32_t flip_x,
    int32_t flip_y, int32_t width, int32_t height, uint32_t adjustment,
    uint32_t token, bool reactive) { return true; }

struct fixture {
    struct np_server server;
    struct wl_client *client;
    struct np_surface *surface;
    int peer;
};
struct events { unsigned char data[8192]; size_t size; };
struct event { const unsigned char *payload; size_t size, index; };

static uint32_t word(const unsigned char *bytes) {
    uint32_t result;
    memcpy(&result, bytes, sizeof(result));
    return result;
}

static struct events drain(struct fixture *f) {
    wl_display_flush_clients(f->server.display);
    struct events events = {0};
    for (;;) {
        ssize_t count = recv(f->peer, events.data + events.size,
                            sizeof(events.data) - events.size, MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (count == 0) break;
        assert(count > 0);
        events.size += (size_t)count;
        assert(events.size < sizeof(events.data));
    }
    return events;
}

static struct event event(const struct events *events, uint32_t object, uint16_t opcode) {
    size_t index = 0;
    for (size_t offset = 0; offset < events->size; index++) {
        assert(events->size - offset >= 8);
        uint32_t header = word(events->data + offset + 4);
        size_t size = header >> 16;
        assert(size >= 8 && size <= events->size - offset);
        if (word(events->data + offset) == object && (uint16_t)header == opcode)
            return (struct event){events->data + offset + 8, size - 8, index};
        offset += size;
    }
    return (struct event){0};
}

static bool has_state(struct event configure, uint32_t state) {
    assert(configure.payload && configure.size >= 12);
    size_t size = word(configure.payload + 8);
    assert(size % 4 == 0 && configure.size == 12 + size);
    for (size_t offset = 12; offset < configure.size; offset += 4)
        if (word(configure.payload + offset) == state) return true;
    return false;
}

static void begin(struct fixture *f, int surface_version, int shell_version) {
    memset(f, 0, sizeof(*f));
    f->server.display = wl_display_create(); assert(f->server.display);
    f->server.next_id = 1;
    f->server.output_scale = 2;
    f->server.output_width = 2880;
    f->server.output_height = 1800;
    wl_list_init(&f->server.surfaces);
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    f->client = wl_client_create(f->server.display, pair[0]); assert(f->client);
    f->peer = pair[1];
    struct wl_resource *compositor = wl_resource_create(
        f->client, &wl_compositor_interface, surface_version, 0); assert(compositor);
    wl_resource_set_user_data(compositor, &f->server);
    compositor_create_surface(f->client, compositor, 2);
    f->surface = np_surface_by_id(&f->server, 1); assert(f->surface);
    if (shell_version) {
        struct wl_resource *shell = wl_resource_create(
            f->client, &xdg_wm_base_interface, shell_version, 0); assert(shell);
        wl_resource_set_user_data(shell, &f->server);
        wm_base_get_xdg_surface(f->client, shell, 3, f->surface->resource);
        assert(f->surface->xdg_surface);
        xdg_surface_get_toplevel(f->client, f->surface->xdg_surface, 4);
        assert(f->surface->toplevel);
    }
}

static void finish(struct fixture *f) {
    wl_client_destroy(f->client);
    close(f->peer);
    wl_display_destroy(f->server.display);
}

static void test_surface_preferences(void) {
    for (int version = 4; version <= 6; version++) {
        struct fixture f;
        begin(&f, version, 0);
        uint32_t id = wl_resource_get_id(f.surface->resource);
        struct events events = drain(&f);
        struct event scale = event(&events, id, WL_SURFACE_PREFERRED_BUFFER_SCALE);
        struct event transform = event(&events, id, WL_SURFACE_PREFERRED_BUFFER_TRANSFORM);
        if (version < 6) assert(!scale.payload && !transform.payload);
        else {
            assert(scale.payload && word(scale.payload) == 2);
            assert(transform.payload && word(transform.payload) == WL_OUTPUT_TRANSFORM_NORMAL);
        }
        np_surface_send_buffer_preferences(f.surface, 2);
        assert(drain(&f).size == 0);
        np_surface_send_buffer_preferences(f.surface, 3);
        events = drain(&f);
        scale = event(&events, id, WL_SURFACE_PREFERRED_BUFFER_SCALE);
        assert(version < 6 ? !scale.payload : scale.payload && word(scale.payload) == 3);
        assert(!event(&events, id, WL_SURFACE_PREFERRED_BUFFER_TRANSFORM).payload);
        /* Preferred density never mutates committed client buffer state. */
        assert(f.surface->scale == 1 && f.surface->transform == WL_OUTPUT_TRANSFORM_NORMAL);
        finish(&f);
    }
}

static void test_offset_versions(void) {
    struct fixture f;
    begin(&f, 4, 0);
    surface_attach(f.client, f.surface->resource, NULL, 7, -4);
    assert(f.surface->pending_buffer_set && f.surface->pending_offset_changed);
    assert(f.surface->pending_offset_x == 7 && f.surface->pending_offset_y == -4);
    finish(&f);
    begin(&f, 5, 0);
    surface_offset(f.client, f.surface->resource, 9, -6);
    surface_attach(f.client, f.surface->resource, NULL, 0, 0);
    assert(f.surface->pending_offset_x == 9 && f.surface->pending_offset_y == -6);
    f.surface->pending_buffer_set = false;
    surface_attach(f.client, f.surface->resource, NULL, 1, 0);
    assert(!f.surface->pending_buffer_set); /* invalid_offset preserves pending state */
    struct events events = drain(&f);
    struct event error = event(&events, 1, WL_DISPLAY_ERROR);
    assert(error.payload && word(error.payload + 4) == WL_SURFACE_ERROR_INVALID_OFFSET);
    finish(&f);
}

static void initial_configure(struct fixture *f) {
    assert(f->surface->xdg_configure_phase == NP_XDG_AWAITING_INITIAL_COMMIT);
    np_xdg_send_initial_role_configure(f->surface);
    f->surface->xdg_configure_phase = NP_XDG_AWAITING_INITIAL_ACK;
}

static void test_shell_versions_and_order(void) {
    for (int version = 3; version <= 6; version++) {
        struct fixture f;
        begin(&f, 6, version);
        (void)drain(&f);
        assert(f.surface->latest_configure_serial == 0);
        initial_configure(&f);
        struct events events = drain(&f);
        struct event configure = event(&events, 4, XDG_TOPLEVEL_CONFIGURE);
        struct event bounds = event(&events, 4, XDG_TOPLEVEL_CONFIGURE_BOUNDS);
        struct event caps = event(&events, 4, XDG_TOPLEVEL_WM_CAPABILITIES);
        struct event serial = event(&events, 3, XDG_SURFACE_CONFIGURE);
        assert(configure.payload && serial.payload && configure.index < serial.index);
        assert(word(configure.payload) == 0 && word(configure.payload + 4) == 0);
        assert(!has_state(configure, XDG_TOPLEVEL_STATE_SUSPENDED));
        if (version < 4) assert(!bounds.payload);
        else {
            assert(bounds.payload && bounds.index < configure.index);
            assert(word(bounds.payload) == 1440 && word(bounds.payload + 4) == 900);
        }
        if (version < 5) assert(!caps.payload);
        else {
            assert(caps.payload && caps.index < serial.index && word(caps.payload) == 12);
            assert(word(caps.payload + 4) == XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE);
            assert(word(caps.payload + 8) == XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
            assert(word(caps.payload + 12) == XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
        }
        np_xdg_host_window_state(f.surface, false, 1400, 850);
        events = drain(&f);
        configure = event(&events, 4, XDG_TOPLEVEL_CONFIGURE);
        if (version < 4) assert(!configure.payload);
        else assert(has_state(configure, XDG_TOPLEVEL_STATE_SUSPENDED) == (version >= 6));
        np_xdg_host_window_state(f.surface, false, 1400, 850);
        assert(drain(&f).size == 0);
        np_xdg_host_window_state(f.surface, true, 1400, 850);
        events = drain(&f);
        configure = event(&events, 4, XDG_TOPLEVEL_CONFIGURE);
        if (version < 6) assert(!configure.payload);
        else assert(configure.payload && !has_state(configure, XDG_TOPLEVEL_STATE_SUSPENDED));
        finish(&f);
    }
}

static void test_bounds_and_resize_mailbox(void) {
    struct fixture f;
    begin(&f, 6, 6);
    (void)drain(&f);
    np_xdg_host_window_state(f.surface, true, 1300, 800);
    assert(f.surface->latest_configure_serial == 0); /* waits for initial commit */
    assert(drain(&f).size == 0);
    initial_configure(&f);
    (void)drain(&f);
    np_xdg_configure_toplevel_from_host(f.surface, 700, 500, NP_CONFIGURE_RESIZING, 123);
    assert(f.surface->host_configure_pending);
    np_xdg_host_window_state(f.surface, false, 1200, 750);
    struct events events = drain(&f);
    struct event configure = event(&events, 4, XDG_TOPLEVEL_CONFIGURE);
    struct event bounds = event(&events, 4, XDG_TOPLEVEL_CONFIGURE_BOUNDS);
    assert(configure.payload && word(configure.payload) == 700 && word(configure.payload + 4) == 500);
    assert(has_state(configure, XDG_TOPLEVEL_STATE_RESIZING));
    assert(has_state(configure, XDG_TOPLEVEL_STATE_SUSPENDED));
    assert(bounds.payload && bounds.index < configure.index && word(bounds.payload) == 1200);
    struct np_xdg_configure *pending = wl_container_of(f.surface->xdg_configures.prev, pending, link);
    assert(pending->host_serial == 123);
    np_xdg_host_window_state(f.surface, false, -1, 10);
    np_xdg_host_window_state(f.surface, false, 0, 10);
    assert(drain(&f).size == 0 && f.surface->host_window_bounds_width == 1200);
    np_xdg_host_window_state(f.surface, true, 0, 0);
    events = drain(&f);
    bounds = event(&events, 4, XDG_TOPLEVEL_CONFIGURE_BOUNDS);
    assert(bounds.payload && word(bounds.payload) == 0 && word(bounds.payload + 4) == 0);
    np_xdg_clear_configures(f.surface);
    assert(!f.surface->host_window_visibility_known && !f.surface->reported_toplevel_capabilities);
    finish(&f);

    begin(&f, 6, 6);
    (void)drain(&f);
    initial_configure(&f);
    (void)drain(&f);
    f.server.output_width = 2560;
    np_xdg_output_bounds_changed(f.surface);
    events = drain(&f);
    bounds = event(&events, 4, XDG_TOPLEVEL_CONFIGURE_BOUNDS);
    configure = event(&events, 4, XDG_TOPLEVEL_CONFIGURE);
    assert(bounds.payload && word(bounds.payload) == 1280 && configure.payload && bounds.index < configure.index);
    finish(&f);
}

int main(void) {
    test_surface_preferences();
    test_offset_versions();
    test_shell_versions_and_order();
    test_bounds_and_resize_mailbox();
    puts("core protocols: version gates, buffer preferences, offsets, bounds/capabilities, host occlusion and resize mailbox PASS");
    return 0;
}
