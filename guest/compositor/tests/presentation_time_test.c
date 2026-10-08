/* Exercise real Wayland feedback resources and destruction with deterministic
 * transport and time. A protocol error is observable separately from the
 * presented/discarded events; unknown display outcomes must not fabricate one. */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

static int fixture_clock_gettime(clockid_t clock, struct timespec *time);
bool np_trace_enabled(void) { return false; }
static void capture_event(struct wl_resource *resource, uint32_t opcode, ...);
static void capture_implementation_error(struct wl_client *client, const char *format, ...);
#define clock_gettime fixture_clock_gettime
#define wl_resource_post_event capture_event
#define wl_client_post_implementation_error capture_implementation_error
#include "../presentation_time.c"
#undef wl_client_post_implementation_error
#undef wl_resource_post_event
#undef clock_gettime
#include "../windowwire.c"
#include <stdio.h>

#define MS UINT64_C(1000000)
#define SECOND UINT64_C(1000000000)
#define FIXTURE_SCENE_SIZE 188u
struct feedback_event {
    uint32_t id, refresh, sequence_hi, sequence_lo, flags;
    uint64_t time;
    bool presented;
};
struct transport_event {
    uint8_t opcode;
    size_t count;
    uint32_t values[6];
    unsigned feedbacks_sent, errors_sent;
};
struct fixture {
    struct np_server server;
    struct np_surface root, child;
    struct wl_client *client;
    struct wl_resource *manager, *root_resource, *child_resource;
    int peer;
    uint32_t next_id, clock_token;
    unsigned clock_requests, clock_ids, output_calls, errors;
    uint64_t now;
    bool boundary_ready, unsupported_child;
    struct feedback_event events[32];
    unsigned event_count;
    struct transport_event transport[64];
    unsigned transport_count;
};
static struct fixture *active;

static int fixture_clock_gettime(clockid_t clock, struct timespec *time)
{
    assert(active && clock == CLOCK_MONOTONIC);
    time->tv_sec = active->now / SECOND;
    time->tv_nsec = active->now % SECOND;
    return 0;
}

static void capture_event(struct wl_resource *resource, uint32_t opcode, ...)
{
    va_list args;
    va_start(args, opcode);
    if (!strcmp(wl_resource_get_class(resource), "wp_presentation")) {
        assert(opcode == WP_PRESENTATION_CLOCK_ID);
        assert(va_arg(args, uint32_t) == CLOCK_MONOTONIC);
        active->clock_ids++;
    } else {
        assert(!strcmp(wl_resource_get_class(resource), "wp_presentation_feedback"));
        assert(active->event_count < 32);
        struct feedback_event *event = &active->events[active->event_count++];
        event->id = wl_resource_get_id(resource);
        if (opcode == WP_PRESENTATION_FEEDBACK_PRESENTED) {
            uint32_t seconds_hi = va_arg(args, uint32_t);
            uint32_t seconds_lo = va_arg(args, uint32_t);
            uint32_t nanoseconds = va_arg(args, uint32_t);
            assert(nanoseconds < SECOND);
            event->presented = true;
            event->time = (((uint64_t)seconds_hi << 32) | seconds_lo) * SECOND + nanoseconds;
            event->refresh = va_arg(args, uint32_t);
            event->sequence_hi = va_arg(args, uint32_t);
            event->sequence_lo = va_arg(args, uint32_t);
            event->flags = va_arg(args, uint32_t);
        } else assert(opcode == WP_PRESENTATION_FEEDBACK_DISCARDED);
    }
    va_end(args);
}

static void capture_implementation_error(struct wl_client *client, const char *format, ...)
{
    assert(active && client == active->client);
    char reason[256];
    va_list args;
    va_start(args, format);
    vsnprintf(reason, sizeof(reason), format, args);
    va_end(args);
    assert(reason[0]);
    active->errors++;
    /* Keep the real Wayland fatal error path as well as observing its cause. */
    wl_client_post_implementation_error(client, "%s", reason);
}

struct np_surface *np_scene_root(struct np_surface *surface)
{
    if (active->unsupported_child && surface == &active->child) return NULL;
    while (surface && surface->parent) surface = surface->parent;
    return surface;
}

void np_scale_presentation_output_for_server(struct np_server *server,
    struct wl_resource *feedback, uint32_t output_id)
{
    assert(server == &active->server && feedback && output_id == 77);
    active->output_calls++;
}

bool np_backend_display_boundary_ready(struct np_server *server)
{
    assert(server == &active->server);
    return active->boundary_ready;
}
uint64_t np_backend_presentation_clock_window(void)
{
    return NP_PRESENTATION_CLOCK_VM_MAX_RTT_NS;
}

static uint64_t words_u64(const uint32_t *values)
{
    return values[0] | (uint64_t)values[1] << 32;
}

bool np_window_event_send(struct np_server *server, uint8_t opcode,
    const uint32_t *values, size_t count)
{
    assert(server == &active->server && active->transport_count < 64);
    struct transport_event *event = &active->transport[active->transport_count++];
    assert(count <= 6);
    event->opcode = opcode;
    event->count = count;
    memcpy(event->values, values, count * sizeof(*values));
    event->feedbacks_sent = active->event_count;
    event->errors_sent = active->errors;
    uint64_t session = np_presentation_time_session(server);
    uint64_t epoch = np_presentation_time_epoch(server);
    switch (opcode) {
    case NP_GUEST_PRESENTATION_CLOCK_REQUESTED:
        assert(count == 5 && values[0] && words_u64(values + 1) == session);
        assert(words_u64(values + 3) == epoch);
        active->clock_token = values[0];
        active->clock_requests++;
        break;
    case NP_GUEST_PRESENTATION_RESULT_ACK:
        assert(count == 6 && words_u64(values) == session && words_u64(values + 2));
        assert(values[4] && values[5]);
        break;
    case NP_GUEST_PRESENTATION_PAUSE_REACHED:
    case NP_GUEST_PRESENTATION_DRAINED:
        assert(count == 3 && words_u64(values) == session && values[2]);
        break;
    case NP_GUEST_PRESENTATION_RESUMED:
        assert(count == 5 && words_u64(values) == session);
        assert(words_u64(values + 2) == epoch && values[4]);
        break;
    default: assert(false);
    }
    return true;
}

static unsigned sent_count(const struct fixture *fixture, uint8_t opcode)
{
    unsigned count = 0;
    for (unsigned i = 0; i < fixture->transport_count; ++i)
        count += fixture->transport[i].opcode == opcode;
    return count;
}

static uint64_t packet_u64(const unsigned char *packet, unsigned offset)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)packet[offset + i] << (8 * i);
    return value;
}

static void fixture_put_u32(unsigned char *packet, unsigned offset, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) packet[offset + i] = value >> (8 * i);
}

static void fixture_put_u64(unsigned char *packet, unsigned offset, uint64_t value)
{
    fixture_put_u32(packet, offset, value);
    fixture_put_u32(packet, offset + 4, value >> 32);
}

static void fixture_put_f32(unsigned char *packet, unsigned offset, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    fixture_put_u32(packet, offset, bits);
}

static void init_scene_packet(unsigned char *packet, uint32_t owner, uint32_t scene)
{
    /* One valid layer and the exact Scene4/100 header used on the display lane. */
    memset(packet, 0, FIXTURE_SCENE_SIZE);
    memcpy(packet, "NPSN", 4);
    packet[4] = 4;
    packet[6] = 100;
    fixture_put_u32(packet, 8, FIXTURE_SCENE_SIZE);
    fixture_put_u32(packet, 12, owner);
    fixture_put_u32(packet, 16, scene);
    fixture_put_u32(packet, 20, 4);
    fixture_put_u32(packet, 24, 4);
    fixture_put_u32(packet, 28, 1);
    fixture_put_u32(packet, 40, 4);
    fixture_put_u32(packet, 44, 4);
    fixture_put_u32(packet, 48, 1);
    fixture_put_u32(packet, 52, 1);
    fixture_put_u32(packet, 64, 4);
    fixture_put_u32(packet, 68, 4);
    fixture_put_u32(packet, 100, owner);
    fixture_put_u32(packet, 104, 42);
    fixture_put_u32(packet, 108, 4);
    fixture_put_u32(packet, 112, 4);
    fixture_put_u32(packet, 116, 16);
    packet[120] = 1; /* BGRA8888. */
    for (unsigned rect = 132; rect <= 164; rect += 16) {
        fixture_put_f32(packet, rect + 8, 4.0f);
        fixture_put_f32(packet, rect + 12, 4.0f);
    }
    fixture_put_f32(packet, 180, 1.0f);
}

static bool prepare_packet(struct fixture *fixture, uint32_t owner, uint32_t scene,
    unsigned char *packet)
{
    init_scene_packet(packet, owner, scene);
    unsigned char prefix[76];
    memcpy(prefix, packet, sizeof(prefix));
    bool ready = np_presentation_time_prepare_scene(&fixture->server,
        owner, scene, packet, FIXTURE_SCENE_SIZE);
    assert(!memcmp(prefix, packet, sizeof(prefix))); /* Only correlation metadata can change. */
    return ready;
}

static void init_surface(struct fixture *fixture, struct np_surface *surface,
    uint32_t id, struct np_surface *parent)
{
    surface->server = &fixture->server;
    surface->id = id;
    surface->parent = parent;
    wl_list_init(&surface->presentation_feedbacks);
    wl_list_insert(fixture->server.surfaces.prev, &surface->link);
}

static void bind_manager(struct fixture *fixture)
{
    assert(fixture->server.presentation_time->global);
    bind_presentation(fixture->client, fixture->server.presentation_time, 1, 4);
    fixture->manager = wl_client_get_object(fixture->client, 4);
    assert(fixture->manager && fixture->clock_ids == 1);
}

static void clock_reply(struct fixture *fixture, uint64_t host)
{
    struct np_window_message message;
    np_window_message_init(&message, NP_WINDOW_HOST_TO_GUEST, NP_HOST_PRESENTATION_CLOCK_SAMPLE);
    np_window_put_u32(&message, fixture->clock_token);
    np_window_put_u64(&message, np_presentation_time_session(&fixture->server));
    np_window_put_u64(&message, np_presentation_time_epoch(&fixture->server));
    np_window_put_u64(&message, host);
    struct np_window_reader reader;
    assert(np_window_reader_init(&reader, message.data, message.len, NP_WINDOW_HOST_TO_GUEST));
    assert(np_presentation_time_handle_command(&fixture->server, &reader));
    np_window_message_clear(&message);
}

static void setup(struct fixture *fixture, bool qualified)
{
    memset(fixture, 0, sizeof(*fixture));
    active = fixture;
    fixture->now = 100 * SECOND;
    fixture->boundary_ready = true;
    fixture->server.display = wl_display_create();
    assert(fixture->server.display);
    fixture->server.host_session_ready = true;
    wl_list_init(&fixture->server.surfaces);
    init_surface(fixture, &fixture->root, 101, NULL);
    init_surface(fixture, &fixture->child, 102, &fixture->root);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    fixture->client = wl_client_create(fixture->server.display, sockets[0]);
    assert(fixture->client);
    fixture->peer = sockets[1];
    fixture->root_resource = wl_resource_create(fixture->client, &wl_surface_interface, 1, 2);
    fixture->child_resource = wl_resource_create(fixture->client, &wl_surface_interface, 1, 3);
    assert(fixture->root_resource && fixture->child_resource);
    wl_resource_set_implementation(fixture->root_resource, NULL, &fixture->root, NULL);
    wl_resource_set_implementation(fixture->child_resource, NULL, &fixture->child, NULL);
    fixture->next_id = 5;
    np_presentation_time_enable(&fixture->server, true);
    assert(fixture->server.presentation_time && fixture->server.presentation_time->timer);
    assert(!fixture->server.presentation_time->global);
    if (qualified) {
        np_presentation_time_clock_request(&fixture->server);
        assert(fixture->clock_token);
        fixture->now += 2 * MS;
        clock_reply(fixture, 900 * SECOND + MS);
        assert(fixture->server.presentation_time->clock.valid);
        bind_manager(fixture);
        fixture->now += 2 * MS;
    }
}

static void teardown(struct fixture *fixture)
{
    np_presentation_time_destroy(&fixture->server);
    assert(wl_list_empty(&fixture->root.presentation_feedbacks));
    assert(wl_list_empty(&fixture->child.presentation_feedbacks));
    wl_display_destroy_clients(fixture->server.display);
    wl_display_destroy(fixture->server.display);
    close(fixture->peer);
    active = NULL;
}

static uint32_t request_feedback(struct fixture *fixture, struct np_surface *surface)
{
    assert(fixture->manager);
    uint32_t id = fixture->next_id++;
    manager_feedback(fixture->client, fixture->manager,
        surface == &fixture->root ? fixture->root_resource : fixture->child_resource, id);
    assert(wl_client_get_object(fixture->client, id));
    return id;
}

static void attach_scene(struct np_surface *surface, uint32_t commit, uint32_t scene)
{
    np_presentation_time_commit(surface, commit);
    np_presentation_time_apply(surface, commit);
    np_presentation_time_scene(surface, commit, scene);
}

static uint64_t submit_scene(struct fixture *fixture, uint32_t owner, uint32_t scene)
{
    unsigned char packet[FIXTURE_SCENE_SIZE];
    assert(prepare_packet(fixture, owner, scene, packet));
    assert(packet_u64(packet, 76) == np_presentation_time_session(&fixture->server));
    assert(packet_u64(packet, 84) == np_presentation_time_epoch(&fixture->server));
    assert(packet_u64(packet, 92) == fixture->now);
    np_presentation_time_submitted(&fixture->server, packet, sizeof(packet));
    return packet_u64(packet, 92);
}

static void submit_scene_with_child(struct fixture *fixture, uint32_t scene)
{
    unsigned char packet[FIXTURE_SCENE_SIZE + 88];
    init_scene_packet(packet, fixture->root.id, scene);
    memcpy(packet + FIXTURE_SCENE_SIZE, packet + 100, 88);
    fixture_put_u32(packet, 8, sizeof(packet));
    fixture_put_u32(packet, 48, 2);
    fixture_put_u32(packet, FIXTURE_SCENE_SIZE, fixture->child.id);
    assert(np_presentation_time_prepare_scene(&fixture->server,
        fixture->root.id, scene, packet, sizeof(packet)));
    np_presentation_time_submitted(&fixture->server, packet, sizeof(packet));
}

static void result(struct fixture *fixture, uint64_t epoch, uint32_t owner,
    uint32_t scene, uint64_t host)
{
    struct np_window_message message;
    np_window_message_init(&message, NP_WINDOW_HOST_TO_GUEST, NP_HOST_PRESENTATION_FEEDBACK);
    np_window_put_u64(&message, np_presentation_time_session(&fixture->server));
    np_window_put_u64(&message, epoch);
    np_window_put_u32(&message, owner);
    np_window_put_u32(&message, scene);
    np_window_put_u64(&message, host);
    np_window_put_u32(&message, 16666667);
    np_window_put_u32(&message, 77);
    struct np_window_reader reader;
    assert(np_window_reader_init(&reader, message.data, message.len, NP_WINDOW_HOST_TO_GUEST));
    assert(np_presentation_time_handle_command(&fixture->server, &reader));
    np_window_message_clear(&message);
}

static void fence_command(struct fixture *fixture, uint8_t opcode, uint64_t session, uint32_t token)
{
    struct np_window_message message;
    np_window_message_init(&message, NP_WINDOW_HOST_TO_GUEST, opcode);
    np_window_put_u64(&message, session);
    np_window_put_u32(&message, token);
    struct np_window_reader reader;
    assert(np_window_reader_init(&reader, message.data, message.len, NP_WINDOW_HOST_TO_GUEST));
    assert(np_presentation_time_handle_command(&fixture->server, &reader));
    np_window_message_clear(&message);
}

static void expect_event(struct fixture *fixture, unsigned index,
    uint32_t id, bool presented, uint64_t time)
{
    assert(index < fixture->event_count);
    const struct feedback_event *event = &fixture->events[index];
    assert(event->id == id && event->presented == presented);
    if (presented) {
        assert(event->time == time);
        assert(event->refresh == 0 && event->sequence_hi == 0 &&
               event->sequence_lo == 0 && event->flags == 0);
    }
    assert(!wl_client_get_object(fixture->client, id));
}

static void initial_clock_gate_leaves_no_query_rendering_available(void)
{
    struct fixture fixture;
    setup(&fixture, false);
    assert(np_presentation_time_session(&fixture.server));
    assert(np_presentation_time_epoch(&fixture.server) == 1);
    /* Internal state and no-query scenes work before a public global exists. */
    submit_scene(&fixture, fixture.root.id, 100);
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    result(&fixture, 1, fixture.root.id, 100, 900 * SECOND + MS);
    assert(!fixture.event_count && sent_count(&fixture, NP_GUEST_PRESENTATION_RESULT_ACK) == 1);
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 50 * MS + 1;
    clock_reply(&fixture, 900 * SECOND + 25 * MS);
    assert(!fixture.server.presentation_time->global && !fixture.server.presentation_time->clock.valid);
    fixture.now = 100 * SECOND + 300 * MS;
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 900 * SECOND + 301 * MS);
    assert(fixture.server.presentation_time->clock.valid);
    bind_manager(&fixture);
    teardown(&fixture);
}

static void disabled_capability_keeps_presentation_private(void)
{
    struct fixture fixture;
    setup(&fixture, false);
    np_presentation_time_enable(&fixture.server, false);
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 900 * SECOND + MS);
    assert(fixture.server.presentation_time->clock.valid);
    assert(!fixture.server.presentation_time->global && !fixture.event_count);
    teardown(&fixture);
}

static void next_commit_binding(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t first = request_feedback(&fixture, &fixture.root);
    uint32_t second = request_feedback(&fixture, &fixture.root);
    assert(np_presentation_time_has_pending(&fixture.root));
    np_presentation_time_commit(&fixture.root, 11);
    assert(!np_presentation_time_has_pending(&fixture.root));
    uint32_t next = request_feedback(&fixture, &fixture.root);
    np_presentation_time_scene(&fixture.root, 11, 501);
    submit_scene(&fixture, fixture.root.id, 501);
    assert(wl_list_length(&fixture.root.presentation_feedbacks) == 1);
    fixture.now = 100 * SECOND + 40 * MS;
    result(&fixture, 1, fixture.root.id, 501, 900 * SECOND + 10 * MS);
    assert(fixture.event_count == 2 && fixture.output_calls == 2);
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t id = fixture.events[i].id;
        assert(id == first || id == second);
        expect_event(&fixture, i, id, true, 100 * SECOND + 10 * MS);
    }
    assert(fixture.events[0].id != fixture.events[1].id);
    assert(wl_client_get_object(fixture.client, next));
    attach_scene(&fixture.root, 12, 502);
    submit_scene(&fixture, fixture.root.id, 502);
    fixture.now = 100 * SECOND + 60 * MS;
    result(&fixture, 1, fixture.root.id, 502, 900 * SECOND + 50 * MS);
    expect_event(&fixture, 2, next, true, 100 * SECOND + 50 * MS);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_RESULT_ACK) == 2);
    teardown(&fixture);
}

static void replacement_preserves_submitted_feedback(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t replaced = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 1001);
    np_presentation_time_apply(&fixture.root, 2);
    expect_event(&fixture, 0, replaced, false, 0);
    uint32_t submitted = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 2, 1002);
    submit_scene(&fixture, fixture.root.id, 1002);
    uint32_t newer = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 3, 1003);
    submit_scene(&fixture, fixture.root.id, 1003);
    assert(fixture.event_count == 1 && wl_client_get_object(fixture.client, submitted));
    fixture.now = 100 * SECOND + 40 * MS;
    result(&fixture, 1, fixture.root.id, 1002, 900 * SECOND + 20 * MS);
    result(&fixture, 1, fixture.root.id, 1003, 900 * SECOND + 30 * MS);
    expect_event(&fixture, 1, submitted, true, 100 * SECOND + 20 * MS);
    expect_event(&fixture, 2, newer, true, 100 * SECOND + 30 * MS);
    result(&fixture, 1, fixture.root.id, 1002, 900 * SECOND + 35 * MS);
    assert(fixture.event_count == 3); /* Replay is ACKed, never redelivered. */
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_RESULT_ACK) == 3);
    teardown(&fixture);
}

static void synchronized_scene_completes_root_and_child_once(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t root = request_feedback(&fixture, &fixture.root);
    uint32_t child = request_feedback(&fixture, &fixture.child);
    attach_scene(&fixture.root, 10, 2001);
    attach_scene(&fixture.child, 11, 2001);
    submit_scene_with_child(&fixture, 2001);
    fixture.now = 100 * SECOND + 40 * MS;
    result(&fixture, 1, fixture.child.id, 2001, 900 * SECOND + 25 * MS);
    assert(fixture.event_count == 0);
    result(&fixture, 1, fixture.root.id, 2001, 900 * SECOND + 25 * MS);
    assert(fixture.event_count == 2 && fixture.output_calls == 2);
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t id = fixture.events[i].id;
        assert(id == root || id == child);
        expect_event(&fixture, i, id, true, 100 * SECOND + 25 * MS);
    }
    assert(fixture.events[0].id != fixture.events[1].id);
    teardown(&fixture);
}

static void submitted_feedback_survives_surface_close_and_reset(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint64_t session = np_presentation_time_session(&fixture.server);
    uint32_t root = request_feedback(&fixture, &fixture.root);
    uint32_t child = request_feedback(&fixture, &fixture.child);
    attach_scene(&fixture.root, 1, 3001);
    attach_scene(&fixture.child, 2, 3001);
    submit_scene_with_child(&fixture, 3001);
    uint32_t pending = request_feedback(&fixture, &fixture.child);
    np_presentation_time_discard_surface(&fixture.root);
    np_presentation_time_discard_surface(&fixture.child);
    expect_event(&fixture, 0, pending, false, 0);
    assert(wl_client_get_object(fixture.client, root) && wl_client_get_object(fixture.client, child));
    wl_resource_destroy(fixture.root_resource);
    wl_resource_destroy(fixture.child_resource);
    fixture.root_resource = fixture.child_resource = NULL;
    wl_list_remove(&fixture.root.link);
    wl_list_remove(&fixture.child.link);
    fixture.server.host_session_ready = false;
    np_presentation_time_reset(&fixture.server);
    assert(fixture.event_count == 1 && np_presentation_time_session(&fixture.server) == session);
    fixture.server.host_session_ready = true;
    fixture.now = 100 * SECOND + 40 * MS;
    result(&fixture, 1, fixture.root.id, 3001, 900 * SECOND + 20 * MS);
    assert(fixture.event_count == 1 && !fixture.output_calls);
    assert(wl_client_get_object(fixture.client, root) && wl_client_get_object(fixture.client, child));
    assert(fixture.server.presentation_time->history_unverified);
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 900 * SECOND + 41 * MS);
    assert(!fixture.server.presentation_time->history_unverified);
    assert(fixture.server.presentation_time->clock.host == 900 * SECOND + 41 * MS);
    assert(fixture.event_count == 3 && fixture.output_calls == 2);
    for (unsigned i = 1; i < 3; ++i) {
        uint32_t id = fixture.events[i].id;
        assert(id == root || id == child);
        expect_event(&fixture, i, id, true, 100 * SECOND + 20 * MS);
    }
    result(&fixture, 1, fixture.root.id, 3001, 900 * SECOND + 30 * MS);
    assert(fixture.event_count == 3);
    teardown(&fixture);
}

static void authoritative_discard_is_terminal(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t declined = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 4001);
    submit_scene(&fixture, fixture.root.id, 4001);
    np_presentation_time_apply(&fixture.root, 2); /* A newer actual commit retires these contents. */
    result(&fixture, 1, fixture.root.id, 4001, 0);
    expect_event(&fixture, 0, declined, false, 0);
    fixture.now += 30 * MS;
    result(&fixture, 1, fixture.root.id, 4001, 900 * SECOND + 10 * MS);
    assert(fixture.event_count == 1 && fixture.output_calls == 0);
    teardown(&fixture);
}

static void client_resource_destruction_and_server_destroy_do_not_invent_events(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t abandoned = request_feedback(&fixture, &fixture.root);
    uint32_t live = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 5001);
    submit_scene(&fixture, fixture.root.id, 5001);
    struct np_display_scene *scene = find_scene(fixture.server.presentation_time, 1, fixture.root.id, 5001);
    assert(scene && wl_list_length(&scene->feedbacks) == 2);
    wl_resource_destroy(wl_client_get_object(fixture.client, abandoned));
    assert(wl_list_length(&scene->feedbacks) == 1 && !fixture.event_count);
    np_presentation_time_discard_commit(&fixture.root, 1);
    result(&fixture, 1, fixture.root.id, 5001, 0);
    expect_event(&fixture, 0, live, false, 0);
    uint32_t unknown = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 2, 5002);
    submit_scene(&fixture, fixture.root.id, 5002);
    np_presentation_time_destroy(&fixture.server);
    assert(fixture.event_count == 1 && !wl_client_get_object(fixture.client, unknown));
    result(&fixture, 1, fixture.root.id, 5002, 900 * SECOND + 20 * MS);
    assert(fixture.event_count == 1);
    teardown(&fixture);
}

static void scene_early_sample_preserves_history_and_rejects_stale_identity(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint64_t session = np_presentation_time_session(&fixture.server);
    uint32_t feedback = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 6001);
    fixture.now = 100 * SECOND + 10 * MS;
    uint64_t sent = submit_scene(&fixture, fixture.root.id, 6001);
    struct np_display_scene *scene = find_scene(fixture.server.presentation_time, 1, fixture.root.id, 6001);
    assert(scene && scene->clock.rtt == 2 * MS);
    fixture.now += MS;
    np_presentation_time_scene_clock_sample(&fixture.server, session + 1, 1,
        fixture.root.id, 6001, sent, 900 * SECOND + 10500 * UINT64_C(1000));
    np_presentation_time_scene_clock_sample(&fixture.server, session, 2,
        fixture.root.id, 6001, sent, 900 * SECOND + 10500 * UINT64_C(1000));
    np_presentation_time_scene_clock_sample(&fixture.server, session, 1,
        fixture.root.id, 6001, sent + 1, 900 * SECOND + 10500 * UINT64_C(1000));
    assert(scene->clock.rtt == 2 * MS);
    np_presentation_time_scene_clock_sample(&fixture.server, session, 1,
        fixture.root.id, 6001, sent, 900 * SECOND + 10500 * UINT64_C(1000));
    assert(scene->clock.rtt == MS && scene->clock.host == 900 * SECOND + 10500 * UINT64_C(1000));
    fixture.now = 100 * SECOND + 300 * MS;
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 900 * SECOND + 301 * MS);
    assert(fixture.server.presentation_time->clock.host == 900 * SECOND + 301 * MS);
    np_presentation_time_result(&fixture.server, session + 1, 1, fixture.root.id, 6001,
        900 * SECOND + 20 * MS, 0, 77);
    result(&fixture, 2, fixture.root.id, 6001, 900 * SECOND + 20 * MS);
    assert(!fixture.event_count && wl_client_get_object(fixture.client, feedback));
    result(&fixture, 1, fixture.root.id, 6001, 900 * SECOND + 20 * MS);
    expect_event(&fixture, 0, feedback, true, 100 * SECOND + 20 * MS);
    teardown(&fixture);
}

static void reset_preserves_unsent_queries_and_replay_binds_only_current_commit(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint64_t session = np_presentation_time_session(&fixture.server);
    uint32_t historical = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 6100);
    submit_scene(&fixture, fixture.root.id, 6100);
    uint32_t feedback = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 2, 6101);
    uint32_t cached = request_feedback(&fixture, &fixture.child);
    np_presentation_time_commit(&fixture.child, 7); /* Cached child state has no scene yet. */
    uint32_t pending = request_feedback(&fixture, &fixture.root);
    struct np_display_feedback *current = wl_resource_get_user_data(wl_client_get_object(fixture.client, feedback));
    struct np_display_feedback *cached_query = wl_resource_get_user_data(wl_client_get_object(fixture.client, cached));
    struct np_display_feedback *pending_query = wl_resource_get_user_data(wl_client_get_object(fixture.client, pending));
    unsigned char packet[FIXTURE_SCENE_SIZE];
    assert(prepare_packet(&fixture, fixture.root.id, 6101, packet));
    np_presentation_time_reset(&fixture.server);
    assert(!fixture.event_count && !fixture.errors);
    assert(np_presentation_time_session(&fixture.server) == session);
    assert(np_presentation_time_epoch(&fixture.server) == 1);
    assert(wl_list_length(&fixture.server.presentation_time->scenes) == 1);
    assert(!np_presentation_time_emission_allowed(&fixture.server));
    assert(current->commit == 2 && current->scene == 6101);
    np_presentation_time_replay_surface(&fixture.root, fixture.root.id, 6102);
    np_presentation_time_replay_surface(&fixture.child, fixture.root.id, 6102);
    assert(current->commit == 2 && current->scene == 6102 && current->owner == fixture.root.id);
    assert(!pending_query->commit && !pending_query->scene);
    assert(cached_query->commit == 7 && !cached_query->scene);
    assert(!prepare_packet(&fixture, fixture.root.id, 6102, packet));
    assert(!prepare_packet(&fixture, fixture.root.id, 6199, packet));
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 900 * SECOND + 5 * MS);
    assert(!fixture.server.presentation_time->reconnecting);
    assert(np_presentation_time_emission_allowed(&fixture.server));
    submit_scene(&fixture, fixture.root.id, 6102);
    fixture.now += 20 * MS;
    result(&fixture, 1, fixture.root.id, 6102, 900 * SECOND + 10 * MS);
    expect_event(&fixture, 0, feedback, true, 100 * SECOND + 10 * MS);
    result(&fixture, 1, fixture.root.id, 6100, 900 * SECOND + 8 * MS);
    expect_event(&fixture, 1, historical, true, 100 * SECOND + 8 * MS);
    assert(wl_client_get_object(fixture.client, cached) && wl_client_get_object(fixture.client, pending));
    assert(!pending_query->commit && !pending_query->scene);
    assert(cached_query->commit == 7 && !cached_query->scene);
    teardown(&fixture);
}

static void calibration_failure_retries_without_fabricating_feedback(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t historical = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 6110);
    submit_scene(&fixture, fixture.root.id, 6110);
    np_presentation_time_reset(&fixture.server);
    uint32_t feedback = request_feedback(&fixture, &fixture.root);
    fixture.now += 50 * MS + 1;
    clock_reply(&fixture, 900 * SECOND + 29 * MS);
    result(&fixture, 1, fixture.root.id, 6110, 900 * SECOND + 10 * MS);
    assert(!fixture.server.presentation_time->clock.valid && !fixture.event_count);
    fixture.now = 100 * SECOND + 300 * MS;
    tick(fixture.server.presentation_time);
    assert(fixture.server.presentation_time->token);
    unsigned requests = fixture.clock_requests;
    fixture.now += 60 * MS;
    clock_reply(&fixture, 900 * SECOND + 330 * MS);
    fixture.now = 104 * SECOND;
    tick(fixture.server.presentation_time);
    ++requests; /* Low-frequency recovery issued a new request, not feedback. */
    assert(!fixture.errors && !fixture.event_count);
    assert(wl_client_get_object(fixture.client, feedback));
    assert(fixture.server.presentation_time->faulted);
    assert(fixture.server.presentation_time->history_unverified);
    assert(wl_client_get_object(fixture.client, historical));
    assert(!fixture.server.presentation_time->reconnecting);
    assert(np_presentation_time_emission_allowed(&fixture.server));
    unsigned char packet[FIXTURE_SCENE_SIZE];
    /* Reconnect failure releases ordinary rendering, while public feedback
     * still cannot be promised without a qualified prior clock. */
    assert(prepare_packet(&fixture, fixture.child.id, 6199, packet));
    assert(wl_list_length(&fixture.server.presentation_time->scenes) == 1);
    assert(fixture.clock_requests == requests);
    fixture.now += SECOND;
    tick(fixture.server.presentation_time);
    np_presentation_time_clock_request(&fixture.server);
    assert(fixture.clock_requests == requests);
    assert(!fixture.event_count && !fixture.errors);
    assert(wl_client_get_object(fixture.client, historical));
    /* Restoring ordinary rendering after 3 seconds does not verify history.
     * A cached positive still unknown at 15 seconds fails truthfully. */
    fixture.now = 100 * SECOND + 4 * MS + RESULT_TIMEOUT_NS;
    tick(fixture.server.presentation_time);
    ++requests;
    assert(fixture.errors == 1 && !fixture.event_count);
    assert(!wl_client_get_object(fixture.client, historical));
    assert(fixture.clock_requests == requests);
    teardown(&fixture);
}

static void remote_initial_clock_and_timeout_recovery_advertise_only_after_a_valid_sample(void)
{
    struct fixture fixture;
    setup(&fixture, false);
    struct np_presentation_time *state = fixture.server.presentation_time;
    state->clock.maximum_rtt_ns = NP_PRESENTATION_CLOCK_REMOTE_MAX_RTT_NS;
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 80 * MS;
    clock_reply(&fixture, 900 * SECOND + 40 * MS);
    assert(state->global && state->clock.valid && state->clock.rtt == 80 * MS);
    bind_manager(&fixture);
    uint32_t feedback = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 6191);
    submit_scene(&fixture, fixture.root.id, 6191);
    fixture.now = 100 * SECOND + 300 * MS;
    result(&fixture, 1, fixture.root.id, 6191, 900 * SECOND + 150 * MS);
    expect_event(&fixture, 0, feedback, true, 100 * SECOND + 150 * MS);
    /* Explicit resume uses the same transport policy in its fresh epoch. */
    uint64_t session = state->session;
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session, 1);
    assert(!state->clock.valid && state->clock.maximum_rtt_ns == NP_PRESENTATION_CLOCK_REMOTE_MAX_RTT_NS);
    fixture.now += 90 * MS;
    clock_reply(&fixture, 900 * SECOND + 345 * MS);
    assert(state->clock.valid && state->clock.rtt == 90 * MS && !state->paused);
    teardown(&fixture);

    setup(&fixture, false);
    state = fixture.server.presentation_time;
    state->clock.maximum_rtt_ns = NP_PRESENTATION_CLOCK_REMOTE_MAX_RTT_NS;
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 250 * MS + 1;
    clock_reply(&fixture, 900 * SECOND + 125 * MS);
    assert(!state->clock.valid && !state->global);
    fixture.now = 103 * SECOND;
    tick(state);
    assert(state->faulted && !state->clock.valid && !state->global);
    unsigned requests = fixture.clock_requests;
    fixture.now += 500 * MS;
    tick(state);
    assert(fixture.clock_requests == requests && state->faulted && !state->global);
    fixture.now = 105 * SECOND;
    tick(state);
    assert(fixture.clock_requests == requests + 1 && state->faulted && !state->global);
    fixture.now += 80 * MS;
    clock_reply(&fixture, 905 * SECOND + 40 * MS);
    assert(state->clock.valid && !state->faulted && state->global);
    assert(!fixture.event_count && !fixture.errors);
    teardown(&fixture);
}

static void slower_remote_validation_extends_history_without_rewriting_saved_mapping(void)
{
    struct fixture fixture;
    setup(&fixture, false);
    struct np_presentation_time *state = fixture.server.presentation_time;
    state->clock.maximum_rtt_ns = NP_PRESENTATION_CLOCK_REMOTE_MAX_RTT_NS;
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 80 * MS;
    clock_reply(&fixture, 900 * SECOND + 40 * MS);
    bind_manager(&fixture);
    fixture.child.parent = NULL; /* Two independently presented toplevels. */
    uint32_t before_fault = request_feedback(&fixture, &fixture.root);
    uint32_t uncertain = request_feedback(&fixture, &fixture.child);
    fixture.now = 100 * SECOND + 100 * MS;
    attach_scene(&fixture.root, 1, 6192);
    attach_scene(&fixture.child, 1, 6193);
    submit_scene(&fixture, fixture.root.id, 6192);
    submit_scene(&fixture, fixture.child.id, 6193);
    struct np_display_scene *saved = find_scene(state, 1, fixture.root.id, 6192);
    uint64_t saved_host = saved->clock.host, saved_guest = saved->clock.guest;
    fixture.now = 103 * SECOND;
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 90 * MS;
    clock_reply(&fixture, 903 * SECOND + 45 * MS);
    assert(state->clock.host == 900 * SECOND + 40 * MS && state->clock.rtt == 80 * MS);
    assert(state->clock.epoch_host == 903 * SECOND + 45 * MS);
    assert(saved->clock.host == saved_host && saved->clock.guest == saved_guest);
    fixture.now = 103 * SECOND + 300 * MS;
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 90 * MS;
    clock_reply(&fixture, 904 * SECOND + 345 * MS); /* An actual offset jump. */
    assert(state->faulted && !state->clock.valid && saved->mapping_ceiling == 903 * SECOND + 45 * MS);
    // The old mapping remains usable up to the later independent validation,
    // even though the better RTT anchor itself was intentionally not replaced.
    result(&fixture, 1, fixture.root.id, 6192, 902 * SECOND);
    expect_event(&fixture, 0, before_fault, true, 102 * SECOND);
    result(&fixture, 1, fixture.child.id, 6193, 903 * SECOND + 100 * MS);
    assert(fixture.errors == 1 && fixture.event_count == 1);
    assert(!wl_client_get_object(fixture.client, uncertain));
    teardown(&fixture);
}

static void actual_queried_render_after_clock_timeout_errors_only_that_attempt(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    np_presentation_time_reset(&fixture.server);
    uint32_t mapped = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 7401);
    fixture.unsupported_child = true;
    uint32_t late_role = request_feedback(&fixture, &fixture.child);
    np_presentation_time_commit(&fixture.child, 2);
    unsigned char packet[FIXTURE_SCENE_SIZE];
    /* An outstanding calibration permits a real queried scene to wait. */
    assert(!prepare_packet(&fixture, fixture.root.id, 7401, packet));
    assert(!fixture.errors && !fixture.event_count);
    fixture.now += 50 * MS + 1;
    clock_reply(&fixture, 900 * SECOND + 29 * MS);
    fixture.now = 104 * SECOND;
    tick(fixture.server.presentation_time);
    assert(fixture.server.presentation_time->faulted && !fixture.server.presentation_time->clock.valid);
    assert(np_presentation_time_emission_allowed(&fixture.server));
    assert(!fixture.errors && !fixture.event_count);
    assert(wl_client_get_object(fixture.client, mapped));
    assert(wl_client_get_object(fixture.client, late_role));
    assert(prepare_packet(&fixture, fixture.child.id, 7499, packet));
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    /* Only an actual mapped rendering attempt is now unable to honour its
     * feedback promise. The timer cannot classify every waiting role this way. */
    assert(!prepare_packet(&fixture, fixture.root.id, 7401, packet));
    assert(fixture.errors == 1 && !fixture.event_count && !fixture.output_calls);
    assert(!wl_client_get_object(fixture.client, mapped));
    assert(wl_client_get_object(fixture.client, late_role));
    assert(prepare_packet(&fixture, fixture.child.id, 7499, packet));
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    teardown(&fixture);
}

static void reconnect_changed_offset_preserves_only_verified_history(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t early = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 7301);
    submit_scene(&fixture, fixture.root.id, 7301);
    uint32_t late = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 2, 7302);
    submit_scene(&fixture, fixture.root.id, 7302);
    uint32_t uncertain = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 3, 7303);
    submit_scene(&fixture, fixture.root.id, 7303);
    np_presentation_time_apply(&fixture.root, 4);
    uint32_t dropped = request_feedback(&fixture, &fixture.child);
    attach_scene(&fixture.child, 4, 7304);
    submit_scene_with_child(&fixture, 7304);
    np_presentation_time_apply(&fixture.child, 5);
    fixture.now = 100 * SECOND + 300 * MS;
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 900 * SECOND + 301 * MS);
    fixture.now += 2 * MS;
    np_presentation_time_reset(&fixture.server);
    assert(fixture.server.presentation_time->history_unverified);
    assert(fixture.server.presentation_time->disconnected_clock.valid);
    result(&fixture, 1, fixture.root.id, 7301, 900 * SECOND + 20 * MS);
    assert(!fixture.event_count && !fixture.output_calls);
    result(&fixture, 1, fixture.root.id, 7304, 0);
    expect_event(&fixture, 0, dropped, false, 0); /* A proven drop needs no clock. */
    fixture.now = 100 * SECOND + 600 * MS;
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 910 * SECOND + 601 * MS);
    assert(!fixture.server.presentation_time->history_unverified);
    assert(fixture.server.presentation_time->clock.valid);
    expect_event(&fixture, 1, early, true, 100 * SECOND + 20 * MS);
    assert(!fixture.errors && wl_client_get_object(fixture.client, late));
    /* The new anchor is later than these actual displays and cannot convert
     * them. Only the retained scene anchors and last-good historical bound can. */
    result(&fixture, 1, fixture.root.id, 7302, 900 * SECOND + 250 * MS);
    expect_event(&fixture, 2, late, true, 100 * SECOND + 250 * MS);
    result(&fixture, 1, fixture.root.id, 7303, 910 * SECOND + 620 * MS);
    assert(fixture.errors == 1 && fixture.event_count == 3 && fixture.output_calls == 2);
    assert(!wl_client_get_object(fixture.client, uncertain));
    assert(fixture.transport[fixture.transport_count - 1].errors_sent == 1);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_RESULT_ACK) == 4);
    teardown(&fixture);
}

static void submitted_unknown_outcome_errors_without_discard(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t unknown = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 6201);
    submit_scene(&fixture, fixture.root.id, 6201);
    fixture.now += RESULT_TIMEOUT_NS;
    tick(fixture.server.presentation_time);
    assert(fixture.errors == 1 && !fixture.event_count);
    assert(!wl_client_get_object(fixture.client, unknown));
    teardown(&fixture);
}

static void legitimate_late_role_keeps_unsubmitted_feedback_alive(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    fixture.unsupported_child = true;
    uint32_t feedback = request_feedback(&fixture, &fixture.child);
    np_presentation_time_commit(&fixture.child, 2);
    fixture.now += CLOCK_TIMEOUT_NS;
    tick(fixture.server.presentation_time);
    assert(!fixture.errors && !fixture.event_count);
    assert(wl_client_get_object(fixture.client, feedback));
    fixture.unsupported_child = false;
    np_presentation_time_scene(&fixture.child, 2, 6202);
    submit_scene_with_child(&fixture, 6202);
    fixture.now += 20 * MS;
    result(&fixture, 1, fixture.root.id, 6202, 903 * SECOND + 10 * MS);
    expect_event(&fixture, 0, feedback, true, 103 * SECOND + 10 * MS);
    teardown(&fixture);
}

static void clock_discontinuity_preserves_trustworthy_history(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t historical = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 6301);
    submit_scene(&fixture, fixture.root.id, 6301);
    uint32_t dropped = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 2, 6302);
    submit_scene(&fixture, fixture.root.id, 6302);
    uint32_t uncertain = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 3, 6303);
    submit_scene(&fixture, fixture.root.id, 6303);
    /* A last good sample proves that these earlier display instants precede
     * the subsequently detected, otherwise unlocated clock discontinuity. */
    fixture.now = 100 * SECOND + 300 * MS;
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 900 * SECOND + 301 * MS);
    fixture.now = 100 * SECOND + 600 * MS;
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 910 * SECOND + 601 * MS);
    assert(!fixture.errors && !fixture.event_count);
    assert(fixture.server.presentation_time->faulted && !fixture.server.presentation_time->clock.valid);
    assert(wl_client_get_object(fixture.client, historical));
    assert(wl_client_get_object(fixture.client, dropped));
    assert(wl_client_get_object(fixture.client, uncertain));
    result(&fixture, 1, fixture.root.id, 6301, 900 * SECOND + 20 * MS);
    expect_event(&fixture, 0, historical, true, 100 * SECOND + 20 * MS);
    result(&fixture, 1, fixture.root.id, 6302, 0);
    expect_event(&fixture, 1, dropped, false, 0);
    result(&fixture, 1, fixture.root.id, 6303, 910 * SECOND + 620 * MS);
    assert(fixture.errors == 1 && fixture.event_count == 2);
    assert(fixture.transport[fixture.transport_count - 1].opcode == NP_GUEST_PRESENTATION_RESULT_ACK);
    assert(fixture.transport[fixture.transport_count - 1].errors_sent == 1);
    assert(!wl_client_get_object(fixture.client, uncertain));
    result(&fixture, 1, fixture.root.id, 6301, 910 * SECOND + 650 * MS);
    assert(fixture.errors == 1 && fixture.event_count == 2);
    teardown(&fixture);
}

static void first_terminal_time_is_not_overwritten_while_unmappable(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t feedback = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 6401);
    submit_scene(&fixture, fixture.root.id, 6401);
    /* Deliberately impossible future timing is retained as unknown, not
     * replaced by a replayed discard or fabricated receive-time timestamp. */
    result(&fixture, 1, fixture.root.id, 6401, 999 * SECOND);
    result(&fixture, 1, fixture.root.id, 6401, 0);
    struct np_display_scene *scene = find_scene(fixture.server.presentation_time, 1, fixture.root.id, 6401);
    assert(scene && scene->terminal && scene->host == 999 * SECOND);
    assert(!fixture.event_count && !sent_count(&fixture, NP_GUEST_PRESENTATION_RESULT_ACK));
    fixture.now += RESULT_TIMEOUT_NS;
    tick(fixture.server.presentation_time);
    assert(fixture.errors == 1 && !fixture.event_count);
    assert(!wl_client_get_object(fixture.client, feedback));
    teardown(&fixture);
}

static void pause_drain_waits_for_boundary_and_applied_outcomes_then_resume_recalibrates(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint64_t session = np_presentation_time_session(&fixture.server);
    uint32_t submitted = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 7001);
    submit_scene(&fixture, fixture.root.id, 7001);
    fixture.boundary_ready = false;
    fence_command(&fixture, NP_HOST_PRESENTATION_PAUSE, session + 1, 88);
    assert(np_presentation_time_emission_allowed(&fixture.server));
    fence_command(&fixture, NP_HOST_PRESENTATION_PAUSE, session, 88);
    assert(!np_presentation_time_emission_allowed(&fixture.server));
    assert(!sent_count(&fixture, NP_GUEST_PRESENTATION_PAUSE_REACHED));
    tick(fixture.server.presentation_time);
    assert(!sent_count(&fixture, NP_GUEST_PRESENTATION_PAUSE_REACHED));
    fixture.boundary_ready = true;
    tick(fixture.server.presentation_time);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_PAUSE_REACHED) == 1);
    uint32_t during_pause = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 2, 7002);
    unsigned char packet[FIXTURE_SCENE_SIZE];
    assert(!prepare_packet(&fixture, fixture.root.id, 7002, packet));
    fence_command(&fixture, NP_HOST_PRESENTATION_DRAIN, session, 87);
    fence_command(&fixture, NP_HOST_PRESENTATION_DRAIN, session, 88);
    assert(!sent_count(&fixture, NP_GUEST_PRESENTATION_DRAINED));
    fixture.now = 100 * SECOND + 20 * MS;
    result(&fixture, 1, fixture.root.id, 7001, 900 * SECOND + 10 * MS);
    expect_event(&fixture, 0, submitted, true, 100 * SECOND + 10 * MS);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_DRAINED) == 1);
    unsigned drained = fixture.transport_count - 1;
    assert(fixture.transport[drained].opcode == NP_GUEST_PRESENTATION_DRAINED);
    assert(fixture.transport[drained - 1].opcode == NP_GUEST_PRESENTATION_RESULT_ACK);
    assert(fixture.transport[drained - 1].feedbacks_sent == 1);
    assert(fixture.transport[drained].values[2] == 88);
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session + 1, 99);
    assert(np_presentation_time_epoch(&fixture.server) == 1);
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session, 99);
    assert(np_presentation_time_epoch(&fixture.server) == 2);
    assert(!fixture.server.presentation_time->clock.valid);
    assert(!np_presentation_time_emission_allowed(&fixture.server));
    assert(!sent_count(&fixture, NP_GUEST_PRESENTATION_RESUMED));
    np_presentation_time_clock_sample(&fixture.server, fixture.clock_token, session, 1,
        900 * SECOND + 21 * MS);
    assert(!fixture.server.presentation_time->clock.valid);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 910 * SECOND + 21 * MS);
    assert(np_presentation_time_emission_allowed(&fixture.server));
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_RESUMED) == 1);
    submit_scene(&fixture, fixture.root.id, 7002);
    fixture.now = 100 * SECOND + 40 * MS;
    result(&fixture, 2, fixture.root.id, 7002, 910 * SECOND + 30 * MS);
    expect_event(&fixture, 1, during_pause, true, 100 * SECOND + 30 * MS);
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session, 99);
    assert(np_presentation_time_epoch(&fixture.server) == 2);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_RESUMED) == 2);
    unsigned pause_acks = sent_count(&fixture, NP_GUEST_PRESENTATION_PAUSE_REACHED);
    fence_command(&fixture, NP_HOST_PRESENTATION_PAUSE, session, 88);
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session, 98);
    assert(np_presentation_time_emission_allowed(&fixture.server));
    assert(np_presentation_time_epoch(&fixture.server) == 2);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_PAUSE_REACHED) == pause_acks);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_RESUMED) == 2);
    fence_command(&fixture, NP_HOST_PRESENTATION_PAUSE, session, 100);
    assert(!np_presentation_time_emission_allowed(&fixture.server));
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session, 99);
    assert(!np_presentation_time_emission_allowed(&fixture.server));
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_RESUMED) == 2);
    assert(np_presentation_time_epoch(&fixture.server) == 2);
    teardown(&fixture);
}

static void reset_starts_new_control_generation_and_keeps_submitted_anchor(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    struct np_presentation_time *state = fixture.server.presentation_time;
    uint64_t session = state->session;
    uint32_t feedback = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 7101);
    submit_scene(&fixture, fixture.root.id, 7101);
    struct np_display_scene *scene = find_scene(state, 1, fixture.root.id, 7101);
    struct np_presentation_clock anchor = scene->clock;
    fence_command(&fixture, NP_HOST_PRESENTATION_PAUSE, session, 100);
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session, 101);
    assert(state->epoch == 2 && state->last_barrier == 101);
    np_presentation_time_reset(&fixture.server);
    assert(state->session == session && state->epoch == 2 && state->reconnecting);
    assert(!state->last_barrier && !state->pause_token && !state->drain_token);
    assert(!state->resume_token && !state->completed_resume && !state->pause_reached);
    assert(!np_presentation_time_emission_allowed(&fixture.server));
    assert(scene->submitted && scene->epoch == 1 && scene->clock.valid);
    assert(scene->clock.host == anchor.host && scene->clock.guest == anchor.guest);
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 900 * SECOND + 5 * MS);
    assert(np_presentation_time_emission_allowed(&fixture.server));
    /* A replacement host starts its barrier counter at one. */
    fence_command(&fixture, NP_HOST_PRESENTATION_PAUSE, session, 1);
    fence_command(&fixture, NP_HOST_PRESENTATION_DRAIN, session, 1);
    assert(state->last_barrier == 1 && state->paused);
    assert(!sent_count(&fixture, NP_GUEST_PRESENTATION_DRAINED));
    fixture.now += 20 * MS;
    result(&fixture, 1, fixture.root.id, 7101, 900 * SECOND + 10 * MS);
    expect_event(&fixture, 0, feedback, true, 100 * SECOND + 10 * MS);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_DRAINED) == 1);
    teardown(&fixture);
}

static void explicit_resume_timeout_stays_paused_and_new_token_can_retry(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint64_t session = np_presentation_time_session(&fixture.server);
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session, 10);
    fixture.now += 50 * MS + 1;
    clock_reply(&fixture, 910 * SECOND + 29 * MS);
    fixture.now = 104 * SECOND;
    tick(fixture.server.presentation_time);
    assert(fixture.server.presentation_time->faulted);
    assert(fixture.server.presentation_time->resume_token == 10);
    assert(!np_presentation_time_emission_allowed(&fixture.server));
    assert(!sent_count(&fixture, NP_GUEST_PRESENTATION_RESUMED));
    unsigned char packet[FIXTURE_SCENE_SIZE];
    assert(!prepare_packet(&fixture, fixture.root.id, 7199, packet));
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session, 11);
    assert(np_presentation_time_epoch(&fixture.server) == 3);
    assert(!fixture.server.presentation_time->faulted);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 914 * SECOND + MS);
    assert(np_presentation_time_emission_allowed(&fixture.server));
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_RESUMED) == 1);
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session, 10);
    assert(np_presentation_time_epoch(&fixture.server) == 3);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_RESUMED) == 1);
    teardown(&fixture);
}

static void tick_flushes_terminal_using_original_anchor_before_timeout(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t feedback = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 7201);
    submit_scene(&fixture, fixture.root.id, 7201);
    result(&fixture, 1, fixture.root.id, 7201, 900 * SECOND + 10 * MS);
    result(&fixture, 1, fixture.root.id, 7201, 900 * SECOND + 12 * MS);
    assert(!fixture.event_count && wl_client_get_object(fixture.client, feedback));
    fixture.now = 100 * SECOND + 10 * MS;
    tick(fixture.server.presentation_time);
    expect_event(&fixture, 0, feedback, true, 100 * SECOND + 10 * MS);
    assert(!fixture.errors && sent_count(&fixture, NP_GUEST_PRESENTATION_RESULT_ACK) == 1);
    tick(fixture.server.presentation_time);
    assert(fixture.event_count == 1 && sent_count(&fixture, NP_GUEST_PRESENTATION_RESULT_ACK) == 1);
    teardown(&fixture);
}

static void rejected_scene_does_not_block_pause_or_steal_same_id_retry_in_new_epoch(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint64_t session = np_presentation_time_session(&fixture.server);
    const uint32_t id = 7501;
    uint32_t feedback_id = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, id);
    fixture.root.scene_dirty = true;
    fixture.root.scene_presentation_id = id;
    unsigned char rejected[FIXTURE_SCENE_SIZE];
    assert(prepare_packet(&fixture, fixture.root.id, id, rejected));
    struct np_display_scene *scene = find_scene(fixture.server.presentation_time, 1, fixture.root.id, id);
    assert(scene && !scene->submitted && wl_list_empty(&scene->feedbacks));
    const unsigned identity_bytes[] = {4, 6, 12, 16, 76, 84, 92};
    for (unsigned i = 0; i < sizeof(identity_bytes) / sizeof(*identity_bytes); ++i) {
        unsigned char stale[FIXTURE_SCENE_SIZE];
        memcpy(stale, rejected, sizeof(stale));
        if (identity_bytes[i] == 92)
            fixture_put_u64(stale, 92, packet_u64(rejected, 92) + 1);
        else stale[identity_bytes[i]] ^= 2;
        np_presentation_time_reject_scene(&fixture.server, stale, sizeof(stale));
        np_presentation_time_submitted(&fixture.server, stale, sizeof(stale));
        assert(find_scene(fixture.server.presentation_time, 1, fixture.root.id, id) == scene);
        assert(!scene->submitted && !fixture.event_count && !fixture.errors);
    }
    /* The scene was definitely never admitted. Remove only its placeholder;
     * the real query and the common sender's dirty/id marker must survive. */
    np_presentation_time_reject_scene(&fixture.server, rejected, sizeof(rejected));
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    assert(wl_client_get_object(fixture.client, feedback_id));
    assert(fixture.root.scene_dirty && fixture.root.scene_presentation_id == id);
    assert(wl_list_length(&fixture.root.presentation_feedbacks) == 1);
    assert(!fixture.event_count && !fixture.errors);
    fence_command(&fixture, NP_HOST_PRESENTATION_PAUSE, session, 10);
    fence_command(&fixture, NP_HOST_PRESENTATION_DRAIN, session, 10);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_PAUSE_REACHED) == 1);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_DRAINED) == 1);
    assert(!fixture.event_count && wl_client_get_object(fixture.client, feedback_id));
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session, 11);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 900 * SECOND + 5 * MS);
    assert(np_presentation_time_epoch(&fixture.server) == 2);
    unsigned char admitted[FIXTURE_SCENE_SIZE];
    assert(prepare_packet(&fixture, fixture.root.id, id, admitted));
    assert(packet_u64(rejected, 84) == 1 && packet_u64(admitted, 84) == 2);
    np_presentation_time_submitted(&fixture.server, rejected, sizeof(rejected));
    scene = find_scene(fixture.server.presentation_time, 2, fixture.root.id, id);
    assert(scene && !scene->submitted && wl_list_length(&fixture.root.presentation_feedbacks) == 1);
    np_presentation_time_submitted(&fixture.server, admitted, sizeof(admitted));
    assert(scene->submitted && wl_list_length(&scene->feedbacks) == 1);
    assert(wl_list_empty(&fixture.root.presentation_feedbacks));
    /* A late rejection cannot turn an admitted frame into a proven discard. */
    np_presentation_time_reject_scene(&fixture.server, rejected, sizeof(rejected));
    np_presentation_time_reject_scene(&fixture.server, admitted, sizeof(admitted));
    assert(find_scene(fixture.server.presentation_time, 2, fixture.root.id, id) == scene);
    fixture.now = 100 * SECOND + 20 * MS;
    result(&fixture, 2, fixture.root.id, id, 900 * SECOND + 10 * MS);
    expect_event(&fixture, 0, feedback_id, true, 100 * SECOND + 10 * MS);
    result(&fixture, 2, fixture.root.id, id, 900 * SECOND + 15 * MS);
    assert(fixture.event_count == 1 && !fixture.errors);
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    teardown(&fixture);
}

static void actual_registry_advertises_v1_only_after_clock_and_accepts_flat_query(void)
{
    struct fixture fixture;
    setup(&fixture, false);
    struct np_presentation_time *state = fixture.server.presentation_time;
    assert(state->capable && !state->global && !state->clock.valid);
    /* Exercise libwayland's actual registry and bind dispatch over its socket,
     * rather than calling bind_presentation directly. IDs 2/3 are the surfaces. */
    uint32_t get_registry[] = { 1, (12u << 16) | 1u, 4 };
    assert(write(fixture.peer, get_registry, sizeof(get_registry)) == (ssize_t)sizeof(get_registry));
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(fixture.server.display), 0) >= 0);
    assert(wl_client_get_object(fixture.client, 4));
    wl_display_flush_clients(fixture.server.display);
    struct pollfd poll_peer = { .fd = fixture.peer, .events = POLLIN };
    assert(poll(&poll_peer, 1, 0) == 0);
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS; clock_reply(&fixture, 900 * SECOND + MS);
    assert(state->global && state->clock.valid);
    wl_display_flush_clients(fixture.server.display);
    assert(poll(&poll_peer, 1, 2000) > 0);
    unsigned char announcement[36];
    assert(read(fixture.peer, announcement, sizeof(announcement)) == (ssize_t)sizeof(announcement));
    assert(scene_packet_u32(announcement) == 4 && scene_packet_u32(announcement + 4) == (36u << 16));
    uint32_t name = scene_packet_u32(announcement + 8);
    assert(name && scene_packet_u32(announcement + 12) == 16);
    assert(!memcmp(announcement + 16, "wp_presentation", 16) && scene_packet_u32(announcement + 32) == 1);
    unsigned char bind[40] = {0};
    fixture_put_u32(bind, 0, 4); fixture_put_u32(bind, 4, 40u << 16);
    fixture_put_u32(bind, 8, name); fixture_put_u32(bind, 12, 16);
    memcpy(bind + 16, "wp_presentation", 16);
    fixture_put_u32(bind, 32, 1); fixture_put_u32(bind, 36, 5);
    assert(write(fixture.peer, bind, sizeof(bind)) == (ssize_t)sizeof(bind));
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(fixture.server.display), 0) >= 0);
    fixture.manager = wl_client_get_object(fixture.client, 5);
    assert(fixture.manager && wl_resource_get_version(fixture.manager) == 1 && fixture.clock_ids == 1);
    uint32_t feedback_request[] = { 5, (16u << 16) | 1u, 3, 6 };
    assert(write(fixture.peer, feedback_request, sizeof(feedback_request)) == (ssize_t)sizeof(feedback_request));
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(fixture.server.display), 0) >= 0);
    assert(wl_client_get_object(fixture.client, 6) && wl_list_length(&fixture.child.presentation_feedbacks) == 1);
    fixture.next_id = 7; fixture.unsupported_child = true;
    attach_scene(&fixture.child, 1, 7601);
    struct np_window_frame frame = { .resource_id = 42, .width = 4, .height = 4, .bytes_per_row = 16,
        .scale = 1, .format = NP_WINDOW_BGRA8888, .source = NP_WINDOW_FRAME_GPU,
        .presentation_id = 7601, .has_presentation_context = true };
    struct np_window_message message;
    np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST, NP_GUEST_COMMITTED);
    np_window_put_u32(&message, fixture.child.id); np_window_put_frame(&message, &frame);
    assert(message.ok && np_presentation_time_prepare_scene(&fixture.server, fixture.child.id, 7601,
        message.data, message.len));
    assert(scene_packet_u32(message.data + 40) == 0x60);
    np_presentation_time_submitted(&fixture.server, message.data, message.len);
    fixture.now += 4 * MS; result(&fixture, 1, fixture.child.id, 7601, 900 * SECOND + 5 * MS);
    expect_event(&fixture, 0, 6, true, 100 * SECOND + 5 * MS);
    assert(!fixture.errors && wl_list_empty(&state->scenes));
    np_window_message_clear(&message);
    teardown(&fixture);
}

static void same_scene_id_does_not_submit_a_target_absent_from_packet_layers(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t root_query = request_feedback(&fixture, &fixture.root);
    uint32_t child_query = request_feedback(&fixture, &fixture.child);
    attach_scene(&fixture.root, 1, 7901);
    attach_scene(&fixture.child, 2, 7901);
    /* The source snapshot contains only root even though both commits had
     * previously been associated with this owner/id. */
    submit_scene(&fixture, fixture.root.id, 7901);
    struct np_display_scene *scene = find_scene(fixture.server.presentation_time,
        1, fixture.root.id, 7901);
    assert(scene && wl_list_length(&scene->feedbacks) == 1);
    assert(wl_list_empty(&fixture.root.presentation_feedbacks));
    assert(wl_list_length(&fixture.child.presentation_feedbacks) == 1);
    struct np_display_feedback *child = wl_resource_get_user_data(
        wl_client_get_object(fixture.client, child_query));
    assert(child && child->surface == &fixture.child && child->surface_id == fixture.child.id);
    assert(child->scene == 7901 && child->owner == fixture.root.id && !child->retired);
    fixture.now = 100 * SECOND + 40 * MS;
    result(&fixture, 1, fixture.root.id, 7901, 900 * SECOND + 10 * MS);
    assert(fixture.event_count == 1 && fixture.output_calls == 1);
    expect_event(&fixture, 0, root_query, true, 100 * SECOND + 10 * MS);
    result(&fixture, 1, fixture.root.id, 7901, 0);
    assert(fixture.event_count == 1 && wl_client_get_object(fixture.client, child_query));
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    fixture.now += 16 * SECOND;
    tick(fixture.server.presentation_time);
    assert(!fixture.errors && fixture.event_count == 1);
    assert(wl_client_get_object(fixture.client, child_query));
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS;
    clock_reply(&fixture, 916 * SECOND + 41 * MS);
    /* This later actual snapshot finally samples the same current child. */
    submit_scene_with_child(&fixture, 7902);
    assert(wl_list_empty(&fixture.child.presentation_feedbacks));
    assert(child->scene == 7902 && !child->surface && !child->retired);
    fixture.now = 116 * SECOND + 60 * MS;
    result(&fixture, 1, fixture.root.id, 7902, 916 * SECOND + 50 * MS);
    assert(fixture.event_count == 2 && fixture.output_calls == 2 && !fixture.errors);
    expect_event(&fixture, 1, child_query, true, 116 * SECOND + 50 * MS);
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    teardown(&fixture);
}

static void current_zero_reattaches_without_discard_and_replay_preserves_first_display(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    struct np_presentation_time *state = fixture.server.presentation_time;
    uint64_t session = state->session;
    uint32_t query = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 7701);
    submit_scene(&fixture, fixture.root.id, 7701);
    unsigned char blocked[FIXTURE_SCENE_SIZE];
    /* The same current source cannot first display on another scene while its
     * existing submitted query still waits for a truthful outcome. */
    assert(!prepare_packet(&fixture, fixture.root.id, 7702, blocked));
    assert(wl_list_length(&state->scenes) == 1);
    assert(wl_list_empty(&fixture.root.presentation_feedbacks));
    assert(!fixture.errors && !fixture.event_count);
    fixture.root.scene_dirty = true;
    fixture.root.scene_presentation_id = 7702;
    fence_command(&fixture, NP_HOST_PRESENTATION_PAUSE, session, 10);
    fence_command(&fixture, NP_HOST_PRESENTATION_DRAIN, session, 10);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_PAUSE_REACHED) == 1);
    assert(!sent_count(&fixture, NP_GUEST_PRESENTATION_DRAINED));
    result(&fixture, 1, fixture.root.id, 7701, 0);
    assert(!fixture.event_count && !fixture.output_calls && !fixture.errors);
    assert(wl_list_empty(&state->scenes));
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_RESULT_ACK) == 1);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_DRAINED) == 1);
    struct np_display_feedback *feedback = wl_resource_get_user_data(
        wl_client_get_object(fixture.client, query));
    assert(feedback && feedback->surface == &fixture.root && feedback->commit == 1);
    assert(feedback->surface_id == fixture.root.id && !feedback->retired);
    assert(feedback->owner == fixture.root.id && feedback->scene == 7702);
    assert(wl_list_length(&fixture.root.presentation_feedbacks) == 1);
    result(&fixture, 1, fixture.root.id, 7701, 900 * SECOND + 10 * MS);
    assert(!fixture.event_count && sent_count(&fixture, NP_GUEST_PRESENTATION_RESULT_ACK) == 2);
    fixture.now += 16 * SECOND;
    tick(state);
    assert(!fixture.event_count && !fixture.errors && wl_client_get_object(fixture.client, query));
    fence_command(&fixture, NP_HOST_PRESENTATION_RESUME, session, 11);
    assert(state->epoch == 2 && !np_presentation_time_emission_allowed(&fixture.server));
    fixture.now += 2 * MS;
    clock_reply(&fixture, 916 * SECOND + 5 * MS);
    assert(np_presentation_time_emission_allowed(&fixture.server));
    np_presentation_time_replay_surface(&fixture.root, fixture.root.id, 7702);
    submit_scene(&fixture, fixture.root.id, 7702);
    fixture.now = 116 * SECOND + 20 * MS;
    result(&fixture, 2, fixture.root.id, 7702, 916 * SECOND + 10 * MS);
    assert(fixture.event_count == 1 && fixture.output_calls == 1 && !fixture.errors);
    expect_event(&fixture, 0, query, true, 116 * SECOND + 10 * MS);
    result(&fixture, 1, fixture.root.id, 7701, 0);
    assert(fixture.event_count == 1 && wl_list_empty(&state->scenes));
    teardown(&fixture);
}

static void retirement_is_per_target_and_positive_history_survives(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    uint32_t old_root = request_feedback(&fixture, &fixture.root);
    uint32_t current_child = request_feedback(&fixture, &fixture.child);
    attach_scene(&fixture.root, 1, 7801);
    attach_scene(&fixture.child, 1, 7801);
    submit_scene_with_child(&fixture, 7801);
    np_presentation_time_apply(&fixture.root, 2); /* Retires root, not its current child. */
    result(&fixture, 1, fixture.root.id, 7801, 0);
    assert(fixture.event_count == 1 && !fixture.errors);
    expect_event(&fixture, 0, old_root, false, 0);
    struct np_display_feedback *child = wl_resource_get_user_data(
        wl_client_get_object(fixture.client, current_child));
    assert(child && child->surface == &fixture.child && child->commit == 1 && child->scene);
    assert(child->surface_id == fixture.child.id && !child->retired);
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    uint32_t next_root = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 2, 7802);
    /* The actual sampled child layer rebinds its current query to this scene;
     * no fresh child commit or synthetic discarded event is necessary. */
    submit_scene_with_child(&fixture, 7802);
    assert(wl_list_empty(&fixture.child.presentation_feedbacks));
    np_presentation_time_apply(&fixture.root, 3);
    np_presentation_time_apply(&fixture.child, 2);
    fixture.now = 100 * SECOND + 30 * MS;
    result(&fixture, 1, fixture.root.id, 7802, 900 * SECOND + 20 * MS);
    assert(fixture.event_count == 3 && fixture.output_calls == 2 && !fixture.errors);
    for (unsigned i = 1; i < 3; ++i) {
        uint32_t id = fixture.events[i].id;
        assert(id == next_root || id == current_child);
        expect_event(&fixture, i, id, true, 100 * SECOND + 20 * MS);
    }
    assert(fixture.events[1].id != fixture.events[2].id);
    teardown(&fixture);

    setup(&fixture, true);
    uint32_t destroyed = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 7803);
    submit_scene(&fixture, fixture.root.id, 7803);
    np_presentation_time_discard_surface(&fixture.root);
    assert(!fixture.event_count && wl_client_get_object(fixture.client, destroyed));
    wl_resource_destroy(fixture.root_resource);
    fixture.root_resource = NULL;
    wl_list_remove(&fixture.root.link);
    result(&fixture, 1, fixture.root.id, 7803, 0);
    expect_event(&fixture, 0, destroyed, false, 0);
    assert(!fixture.errors && wl_list_empty(&fixture.server.presentation_time->scenes));
    teardown(&fixture);
}

static void malformed_commands_do_not_mutate_presentation_state(void)
{
    struct fixture fixture;
    setup(&fixture, true);
    const uint8_t opcodes[] = {
        NP_HOST_PRESENTATION_CLOCK_SAMPLE, NP_HOST_PRESENTATION_FEEDBACK,
        NP_HOST_SCENE_CLOCK_SAMPLE, NP_HOST_PRESENTATION_PAUSE,
        NP_HOST_PRESENTATION_DRAIN, NP_HOST_PRESENTATION_RESUME,
    };
    struct np_presentation_time *state = fixture.server.presentation_time;
    uint64_t session = state->session, epoch = state->epoch;
    unsigned sent = fixture.transport_count;
    for (unsigned i = 0; i < sizeof(opcodes); ++i) {
        struct np_window_message message;
        uint8_t opcode = opcodes[i];
        np_window_message_init(&message, NP_WINDOW_HOST_TO_GUEST, opcode);
        if (opcode == NP_HOST_PRESENTATION_CLOCK_SAMPLE)
            np_window_put_u32(&message, 7);
        np_window_put_u64(&message, session ^ 1); /* A valid but stale full message is harmless. */
        if (opcode == NP_HOST_PRESENTATION_CLOCK_SAMPLE || opcode == NP_HOST_PRESENTATION_FEEDBACK ||
            opcode == NP_HOST_SCENE_CLOCK_SAMPLE) {
            np_window_put_u64(&message, epoch);
            if (opcode != NP_HOST_PRESENTATION_CLOCK_SAMPLE) {
                np_window_put_u32(&message, fixture.root.id);
                np_window_put_u32(&message, 8001);
            }
            np_window_put_u64(&message, 900 * SECOND);
            if (opcode == NP_HOST_PRESENTATION_FEEDBACK) {
                np_window_put_u32(&message, 16666667);
                np_window_put_u32(&message, 77);
            } else if (opcode == NP_HOST_SCENE_CLOCK_SAMPLE)
                np_window_put_u64(&message, 900 * SECOND + MS);
        } else np_window_put_u32(&message, 7);
        size_t expected = opcode == NP_HOST_PRESENTATION_CLOCK_SAMPLE ? 36 :
            (opcode == NP_HOST_PRESENTATION_FEEDBACK || opcode == NP_HOST_SCENE_CLOCK_SAMPLE ? 48 : 20);
        assert(message.ok && message.len == expected);
        /* Every truncated body and a trailing extra field are rejected before
         * a token, epoch, fence or feedback can be consumed. */
        for (size_t length = 8; length < message.len; ++length) {
            struct np_window_reader reader;
            assert(np_window_reader_init(&reader, message.data, length, NP_WINDOW_HOST_TO_GUEST));
            assert(!np_presentation_time_handle_command(&fixture.server, &reader));
        }
        struct np_window_reader reader;
        assert(np_window_reader_init(&reader, message.data, message.len, NP_WINDOW_HOST_TO_GUEST));
        assert(np_presentation_time_handle_command(&fixture.server, &reader));
        np_window_put_u32(&message, 0);
        assert(np_window_reader_init(&reader, message.data, message.len, NP_WINDOW_HOST_TO_GUEST));
        assert(!np_presentation_time_handle_command(&fixture.server, &reader));
        np_window_message_clear(&message);
        assert(state->session == session && state->epoch == epoch && !state->paused);
        assert(!state->resume_token && !state->drain_token && !state->pause_token);
        assert(fixture.transport_count == sent && !fixture.event_count && !fixture.errors);
    }
    teardown(&fixture);
}

int main(void)
{
    initial_clock_gate_leaves_no_query_rendering_available();
    disabled_capability_keeps_presentation_private();
    next_commit_binding();
    replacement_preserves_submitted_feedback();
    synchronized_scene_completes_root_and_child_once();
    submitted_feedback_survives_surface_close_and_reset();
    authoritative_discard_is_terminal();
    client_resource_destruction_and_server_destroy_do_not_invent_events();
    scene_early_sample_preserves_history_and_rejects_stale_identity();
    reset_preserves_unsent_queries_and_replay_binds_only_current_commit();
    calibration_failure_retries_without_fabricating_feedback();
    remote_initial_clock_and_timeout_recovery_advertise_only_after_a_valid_sample();
    slower_remote_validation_extends_history_without_rewriting_saved_mapping();
    actual_queried_render_after_clock_timeout_errors_only_that_attempt();
    reconnect_changed_offset_preserves_only_verified_history();
    submitted_unknown_outcome_errors_without_discard();
    legitimate_late_role_keeps_unsubmitted_feedback_alive();
    clock_discontinuity_preserves_trustworthy_history();
    first_terminal_time_is_not_overwritten_while_unmappable();
    pause_drain_waits_for_boundary_and_applied_outcomes_then_resume_recalibrates();
    reset_starts_new_control_generation_and_keeps_submitted_anchor();
    explicit_resume_timeout_stays_paused_and_new_token_can_retry();
    tick_flushes_terminal_using_original_anchor_before_timeout();
    rejected_scene_does_not_block_pause_or_steal_same_id_retry_in_new_epoch();
    actual_registry_advertises_v1_only_after_clock_and_accepts_flat_query();
    same_scene_id_does_not_submit_a_target_absent_from_packet_layers();
    current_zero_reattaches_without_discard_and_replay_preserves_first_display();
    retirement_is_per_target_and_positive_history_survives();
    malformed_commands_do_not_mutate_presentation_state();
    puts("Presentation commit/scene ownership, clock admission, truthful terminal lifecycle and pause fences: PASS");
    return 0;
}
