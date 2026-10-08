/* Drive the production frontend, clock ledger and remote scheduler together.
 * Only resource import/encoding and a precisely selected socket write fail. */
#define main presentation_time_fixture_checks_main
#include "presentation_time_test.c"
#undef main
#define np_window_event_send production_window_event_send
#include "../window_events.c"
#undef np_window_event_send

static ssize_t admission_send(int fd, const void *bytes, size_t size, int flags);
#define send admission_send
#include "../hostlink.c"
#undef send
#define clock_gettime fixture_clock_gettime
#include "../presentation.c"
#include "../backends/remote/presentation.c"
#include "../backends/remote/scene.c"
#undef clock_gettime

static struct np_host vm_host;
static bool use_vm;
static unsigned writes, fail_write, holds, releases, media_checks, reject_media_check, displayed;
static unsigned char displayed_packet[188];
static size_t displayed_size;
struct np_encoder { np_encoder_done_fn done; void *user; unsigned char *pixels; };

static ssize_t admission_send(int fd, const void *bytes, size_t size, int flags)
{
    if (++writes == fail_write) { errno = EPIPE; return -1; }
    ssize_t written = send(fd, bytes, size, flags);
    assert(written == (ssize_t)size);
    return written;
}
bool np_trace_enabled(void) { return false; }
bool np_host_transport_connected(const struct np_host *host) { return host->conn_fd >= 0; }
bool np_backend_connected(const struct np_server *server)
{ (void)server; return !use_vm || vm_host.conn_fd >= 0; }
bool np_backend_admit_scene(struct np_server *server, const void *packet, size_t size)
{ return use_vm ? np_host_admit_scene(&vm_host, packet, size) : np_remote_submit_scene(server, packet, size); }
struct np_surface *np_surface_by_id(struct np_server *server, uint32_t id)
{
    struct np_surface *surface;
    wl_list_for_each(surface, &server->surfaces, link) if (surface->id == id) return surface;
    return NULL;
}
void np_surface_apply_unblocked(struct np_surface *surface) { (void)surface; }
bool np_surface_watch_wait_fd(struct np_server *server, int fd, struct wl_event_source **source, int *stored)
{ (void)server; (void)fd; (void)source; (void)stored; assert(false); return false; }
void np_surface_schedule_retry(struct np_server *server) { (void)server; assert(false); }
void np_perf_count(enum np_perf_stage stage) { (void)stage; }
void np_scene_damage_sent(struct np_surface *surface) { (void)surface; }
void np_scene_presented(struct np_surface *surface, uint32_t scene)
{ assert(surface && scene && holds); --holds; ++releases; }
enum np_scene_build_result np_scene_hold_current(struct np_surface *surface, uint32_t scene, int *wait_fd)
{ (void)wait_fd; assert(surface->has_published && scene); ++holds; return NP_SCENE_READY; }
enum np_scene_build_result np_scene_build(struct np_surface *surface, uint32_t scene,
    struct np_scene_packet *packet, int *wait_fd)
{
    (void)wait_fd;
    packet->size = sizeof(displayed_packet);
    packet->data = calloc(1, packet->size);
    assert(packet->data);
    unsigned char *p = packet->data;
    init_scene_packet(p, surface->id, scene);
    fixture_put_u32(p, 104, surface->last_resource_id);
    ++holds;
    return NP_SCENE_READY;
}
int32_t np_scale_surface_refresh_millihz(const struct np_surface *surface)
{ (void)surface; return 60000; }
void np_xdg_flush_pending_toplevel_configure(struct np_surface *surface) { (void)surface; }
void np_media_wake(struct np_media *media) { (void)media; }
bool np_media_can_encode(struct np_media *media)
{ (void)media; return ++media_checks != reject_media_check; }
bool np_media_send_display(struct np_media *media, const void *packet, size_t size)
{
    (void)media;
    assert(size <= sizeof(displayed_packet));
    memcpy(displayed_packet, packet, size);
    displayed_size = size;
    ++displayed;
    return true;
}
bool np_media_send(struct np_media *media, uint8_t codec, uint8_t flags, uint32_t surface,
    uint32_t resource, uint16_t width, uint16_t height, uint64_t pts, uint16_t epoch,
    const uint8_t *bytes, uint32_t size)
{
    (void)media; (void)codec; (void)flags; (void)surface; (void)resource;
    (void)width; (void)height; (void)pts; (void)epoch; (void)bytes; (void)size;
    return true;
}
struct np_encoder *np_encoder_create(int width, int height, bool h264, np_encoder_output_fn output,
    np_encoder_done_fn done, void *user)
{
    (void)width; (void)height; (void)h264; (void)output;
    struct np_encoder *encoder = calloc(1, sizeof(*encoder));
    assert(encoder); encoder->done = done; encoder->user = user; return encoder;
}
bool np_encoder_take_bgra(struct np_encoder *encoder, uint8_t *pixels, int width, int height,
    int stride, uint64_t pts, uint32_t resource, uint8_t flags)
{
    (void)width; (void)height; (void)stride; (void)pts; (void)resource; (void)flags;
    assert(!encoder->pixels); encoder->pixels = pixels; return true;
}
void np_encoder_destroy(struct np_encoder *encoder) { assert(!encoder->pixels); free(encoder); }
uint16_t np_encoder_epoch(const struct np_encoder *encoder) { (void)encoder; return 1; }

static void initialize_publication(struct fixture *fixture, struct np_remote_backend *backend)
{
    setup(fixture, true);
    memset(backend, 0, sizeof(*backend)); fixture->server.backend_state = backend;
    fixture->root.resource = fixture->root_resource;
    fixture->child.resource = fixture->child_resource;
    wl_list_init(&fixture->root.pending_frame_callbacks);
    wl_list_init(&fixture->child.pending_frame_callbacks);
    fixture->root.scene_wait_fd = fixture->child.scene_wait_fd = -1;
    fixture->root.scale = fixture->child.scale = 1;
    unsigned char pixels[64] = {0};
    assert(encode_remote_pixels(&fixture->root, pixels, 4, 4, 16, WL_SHM_FORMAT_XRGB8888));
    writes = holds = releases = media_checks = displayed = 0;
    fail_write = reject_media_check = 0;
    displayed_size = 0;
}
static void initialize_flat(struct fixture *fixture, enum np_surface_role role)
{
    fixture->unsupported_child = true;
    fixture->child.role = role;
    if (role == NP_SURFACE_ROLE_CURSOR) fixture->server.cursor_surface = fixture->child_resource;
    if (role == NP_SURFACE_ROLE_DRAG_ICON) fixture->server.drag_icon = fixture->child_resource;
    unsigned char pixels[64] = {0};
    assert(encode_remote_pixels(&fixture->child, pixels, 4, 4, 16, WL_SHM_FORMAT_XRGB8888));
}
static int initialize_socket(void)
{
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    vm_host = (struct np_host){ .listen_fd = -1, .conn_fd = sockets[0], .output_enabled = true };
    unsigned char notification[8] = { 'N', 'P', 'W', '2', 1, NP_GUEST_NOTIFICATION_BACKLOG_RESET };
    assert(np_notification_outbox_send(&vm_host.notifications, notification, sizeof(notification)));
    fail_write = 2; /* The complete structural record writes; the optional record fails. */
    return sockets[1];
}
static void vm_admission_retains_source_after_optional_write_failure(bool queried)
{
    struct fixture fixture; struct np_remote_backend backend;
    initialize_publication(&fixture, &backend); use_vm = true;
    int peer = initialize_socket();
    uint32_t query = queried ? request_feedback(&fixture, &fixture.root) : 0;
    if (queried) attach_scene(&fixture.root, 1, 61);
    fixture.root.scene_dirty = true; fixture.root.scene_presentation_id = 61;
    np_presentation_flush(&fixture.server);
    assert(writes == 2 && vm_host.conn_fd < 0 && holds == 1 && !releases);
    assert(!fixture.root.scene_dirty && !fixture.root.scene_presentation_id);
    unsigned char stream[200];
    assert(read(peer, stream, sizeof(stream)) == (ssize_t)sizeof(stream));
    assert(!memcmp(stream, "NPIP", 4) && !memcmp(stream + 12, "NPSN", 4));
    assert(packet_u64(stream + 12, 76) == np_presentation_time_session(&fixture.server));
    uint64_t epoch = packet_u64(stream + 12, 84);
    struct np_display_scene *record = find_scene(fixture.server.presentation_time, epoch, fixture.root.id, 61);
    assert(queried ? record && record->submitted && record->sent == packet_u64(stream + 12, 92) : !record);
    fixture.now += MS;
    result(&fixture, epoch, fixture.root.id, 61, 900 * SECOND + 4 * MS);
    if (queried) expect_event(&fixture, 0, query, true, 100 * SECOND + 4 * MS);
    assert(holds == 1 && !releases); /* Actual display does not retire a pixel-read hold. */
    np_presentation_process_released(&fixture.server, fixture.root.id, 61);
    assert(!holds && releases == 1);
    np_backend_surface_destroy(&fixture.root);
    np_host_finish(&vm_host); close(peer); teardown(&fixture);
}
static void vm_flat_frame_admission_retains_source_after_optional_write_failure(void)
{
    struct fixture fixture; struct np_remote_backend backend;
    initialize_publication(&fixture, &backend); use_vm = true;
    initialize_flat(&fixture, NP_SURFACE_ROLE_CURSOR);
    int peer = initialize_socket();
    struct np_surface *surface = &fixture.child;
    uint32_t query = request_feedback(&fixture, surface);
    attach_scene(surface, 62, 62);
    assert(np_presentation_queue_last(surface, 62));
    size_t size = surface->pending_frame_size;
    assert(size == 72 && holds == 1);
    np_presentation_flush(&fixture.server);
    assert(writes == 2 && vm_host.conn_fd < 0 && holds == 1 && !releases && !surface->pending_frame);
    unsigned char stream[84]; assert(read(peer, stream, sizeof(stream)) == (ssize_t)sizeof(stream));
    assert(!memcmp(stream, "NPIP", 4) && stream[17] == NP_GUEST_COMMITTED);
    struct np_presentation_identity identity;
    assert(np_presentation_time_packet_identity(stream + 12, size, &identity));
    assert(scene_packet_u32(stream + 12 + 40) == 0x60);
    assert(identity.owner == surface->id && identity.presentation == 62 && identity.epoch == 1);
    struct np_display_scene *record = find_scene(fixture.server.presentation_time, 1, surface->id, 62);
    assert(record && record->submitted && wl_list_length(&record->feedbacks) == 1);
    fixture.now += MS; result(&fixture, 1, surface->id, 62, 900 * SECOND + 4 * MS);
    expect_event(&fixture, 0, query, true, 100 * SECOND + 4 * MS);
    np_presentation_process_released(&fixture.server, surface->id, 62);
    assert(!holds && releases == 1);
    np_backend_surface_destroy(&fixture.root);
    np_backend_surface_destroy(surface);
    np_host_finish(&vm_host); close(peer); teardown(&fixture);
}
static void remote_backpressure_does_not_leave_a_pause_ghost_or_old_epoch_owner(void)
{
    struct fixture fixture; struct np_remote_backend backend;
    initialize_publication(&fixture, &backend); use_vm = false;
    uint32_t query = request_feedback(&fixture, &fixture.root);
    attach_scene(&fixture.root, 1, 63);
    fixture.root.scene_dirty = true; fixture.root.scene_presentation_id = 63;
    reject_media_check = 2; /* Ready before build, backpressured at actual admission. */
    np_presentation_flush(&fixture.server);
    assert(media_checks == 2 && !backend.jobs[0] && !backend.jobs[1]);
    assert(!holds && releases == 1 && fixture.root.scene_dirty && fixture.root.scene_presentation_id == 63);
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    assert(!wl_list_empty(&fixture.root.presentation_feedbacks) && !fixture.event_count && !fixture.errors);
    uint64_t session = np_presentation_time_session(&fixture.server);
    np_presentation_time_pause(&fixture.server, session, 1);
    np_presentation_time_drain(&fixture.server, session, 1);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_PAUSE_REACHED) == 1);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_DRAINED) == 1);
    np_presentation_time_resume(&fixture.server, session, 2);
    fixture.now += 2 * MS; clock_reply(&fixture, 900 * SECOND + 5 * MS);
    assert(np_presentation_time_epoch(&fixture.server) == 2 && np_presentation_time_emission_allowed(&fixture.server));
    np_presentation_flush(&fixture.server);
    assert(backend.jobs[0] && !fixture.root.scene_dirty && holds == 1);
    struct np_display_scene *record = find_scene(fixture.server.presentation_time, 2, fixture.root.id, 63);
    assert(record && record->submitted && wl_list_length(&fixture.server.presentation_time->scenes) == 1);
    np_remote_flush_encoded(&fixture.server);
    struct np_encoder *encoder = remote_surface(&fixture.root, false)->encoder;
    assert(encoder && encoder->pixels); free(encoder->pixels); encoder->pixels = NULL; encoder->done(encoder->user, true);
    fixture.now += MS; np_remote_flush_encoded(&fixture.server);
    assert(displayed == 1 && !backend.jobs[0] && !backend.jobs[1]);
    assert(packet_u64(displayed_packet, 76) == session && packet_u64(displayed_packet, 84) == 2);
    result(&fixture, 1, fixture.root.id, 63, 0); /* An old epoch cannot consume the new query. */
    assert(!fixture.event_count && find_scene(fixture.server.presentation_time, 2, fixture.root.id, 63));
    fixture.now += MS; result(&fixture, 2, fixture.root.id, 63, 900 * SECOND + 7 * MS);
    expect_event(&fixture, 0, query, true, 100 * SECOND + 7 * MS);
    result(&fixture, 2, fixture.root.id, 63, 900 * SECOND + 7 * MS);
    assert(fixture.event_count == 1 && !fixture.errors && wl_list_empty(&fixture.server.presentation_time->scenes));
    np_presentation_process_released(&fixture.server, fixture.root.id, 63);
    assert(!holds && releases == 2);
    np_backend_surface_destroy(&fixture.root); np_remote_finish_scenes(&fixture.server); teardown(&fixture);
}
static void flat_pre_role_supersession_and_destroy_have_truthful_outcomes(void)
{
    struct fixture fixture; struct np_remote_backend backend;
    initialize_publication(&fixture, &backend); initialize_flat(&fixture, NP_SURFACE_ROLE_NONE); use_vm = true;
    uint32_t old_query = request_feedback(&fixture, &fixture.child);
    attach_scene(&fixture.child, 64, 64);
    assert(np_presentation_queue_last(&fixture.child, 64));
    np_presentation_flush(&fixture.server);
    fixture.now += 60 * SECOND; tick(fixture.server.presentation_time);
    assert(fixture.child.pending_frame && holds == 1 && !fixture.event_count && !fixture.errors);
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    uint32_t current_query = request_feedback(&fixture, &fixture.child);
    attach_scene(&fixture.child, 65, 65);
    assert(np_presentation_queue_last(&fixture.child, 65));
    expect_event(&fixture, 0, old_query, false, 0);
    assert(holds == 1 && releases == 1 && fixture.child.pending_presentation_id == 65);
    fixture.child.role = NP_SURFACE_ROLE_CURSOR; fixture.server.cursor_surface = fixture.child_resource;
    fixture.now += 2 * MS; clock_reply(&fixture, 960 * SECOND + 5 * MS);
    int peer = initialize_socket();
    np_presentation_flush(&fixture.server);
    assert(!fixture.child.pending_frame && holds == 1 && vm_host.conn_fd < 0);
    np_presentation_time_discard_surface(&fixture.child); /* Destruction must not discard submitted work. */
    assert(fixture.event_count == 1);
    fixture.now += MS; result(&fixture, 1, fixture.child.id, 65, 960 * SECOND + 6 * MS);
    expect_event(&fixture, 1, current_query, true, 160 * SECOND + 6 * MS);
    np_presentation_process_released(&fixture.server, fixture.child.id, 65);
    assert(!holds && releases == 2 && !fixture.errors);
    np_backend_surface_destroy(&fixture.root); np_backend_surface_destroy(&fixture.child);
    np_host_finish(&vm_host); close(peer); teardown(&fixture);
}
static void flat_remote_rejection_pause_resume_and_actual_time_use_new_epoch(void)
{
    struct fixture fixture; struct np_remote_backend backend;
    initialize_publication(&fixture, &backend); initialize_flat(&fixture, NP_SURFACE_ROLE_DRAG_ICON); use_vm = false;
    uint32_t query = request_feedback(&fixture, &fixture.child);
    attach_scene(&fixture.child, 66, 66);
    assert(np_presentation_queue_last(&fixture.child, 66));
    unsigned char *candidate = fixture.child.pending_frame;
    reject_media_check = 2;
    np_presentation_flush(&fixture.server);
    assert(media_checks == 2 && fixture.child.pending_frame == candidate && holds == 1 && !releases);
    assert(!backend.jobs[0] && !backend.jobs[1] && wl_list_empty(&fixture.server.presentation_time->scenes));
    uint64_t session = np_presentation_time_session(&fixture.server);
    np_presentation_time_pause(&fixture.server, session, 1);
    np_presentation_time_drain(&fixture.server, session, 1);
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_DRAINED) == 1 && !fixture.event_count);
    np_presentation_time_resume(&fixture.server, session, 2);
    fixture.now += 2 * MS; clock_reply(&fixture, 900 * SECOND + 5 * MS);
    np_presentation_flush(&fixture.server);
    assert(backend.jobs[0] && !fixture.child.pending_frame && holds == 1 && !releases);
    struct np_display_scene *record = find_scene(fixture.server.presentation_time, 2, fixture.child.id, 66);
    assert(record && record->submitted);
    np_remote_flush_encoded(&fixture.server);
    struct np_encoder *encoder = remote_surface(&fixture.child, false)->encoder;
    assert(encoder && encoder->pixels); free(encoder->pixels); encoder->pixels = NULL; encoder->done(encoder->user, true);
    fixture.now += MS; np_remote_flush_encoded(&fixture.server);
    struct np_presentation_identity identity;
    assert(displayed == 1 && displayed_size == 72);
    assert(np_presentation_time_packet_identity(displayed_packet, displayed_size, &identity));
    assert(identity.session == session && identity.epoch == 2 && identity.owner == fixture.child.id &&
        identity.presentation == 66 && identity.sent == fixture.now);
    assert(scene_packet_u32(displayed_packet + 40) == 0x60);
    result(&fixture, 1, fixture.child.id, 66, 0);
    assert(!fixture.event_count && find_scene(fixture.server.presentation_time, 2, fixture.child.id, 66) == record);
    fixture.now += MS; result(&fixture, 2, fixture.child.id, 66, 900 * SECOND + 7 * MS);
    expect_event(&fixture, 0, query, true, 100 * SECOND + 7 * MS);
    result(&fixture, 2, fixture.child.id, 66, 0);
    assert(fixture.event_count == 1 && !fixture.errors);
    np_presentation_process_released(&fixture.server, fixture.child.id, 66);
    assert(!holds && releases == 1);
    np_backend_surface_destroy(&fixture.root); np_backend_surface_destroy(&fixture.child);
    np_remote_finish_scenes(&fixture.server); teardown(&fixture);
}
static void flat_clock_context_follows_all_variable_frame_fields(void)
{
    struct fixture fixture; struct np_remote_backend backend;
    initialize_publication(&fixture, &backend); initialize_flat(&fixture, NP_SURFACE_ROLE_CURSOR);
    struct np_window_rect damage = {0, 0, 4, 4};
    struct np_window_frame frame = { .resource_id = 42, .width = 4, .height = 4, .bytes_per_row = 16,
        .scale = 1, .format = NP_WINDOW_BGRA8888, .source = NP_WINDOW_FRAME_ENCODED, .presentation_id = 67,
        .has_viewport_source = true, .viewport_source = {0, 0, 4, 4},
        .has_viewport_destination = true, .viewport_width = 4, .viewport_height = 4,
        .has_window_geometry = true, .window_geometry = {0, 0, 4, 4}, .codec = "av1",
        .has_gpu_source = true, .gpu_source_id = 43, .damage = &damage, .damage_count = 1 };
    unsigned char *packet; size_t size;
    assert(np_window_event_send_frame(&fixture.server, fixture.child.id, &frame, &packet, &size));
    assert(scene_packet_u32(packet + 40) == 0x3f && size == 155);
    struct np_presentation_identity identity;
    assert(!np_presentation_time_packet_identity(packet, size, &identity)); /* Reserved but not admitted yet. */
    assert(np_presentation_time_prepare_scene(&fixture.server, fixture.child.id, 67, packet, size));
    assert(scene_packet_u32(packet + 40) == 0x3f); /* No query keeps the native cursor path available. */
    assert(np_presentation_time_packet_identity(packet, size, &identity));
    assert(identity.session == np_presentation_time_session(&fixture.server) && identity.epoch == 1 &&
        identity.sent == fixture.now && identity.owner == fixture.child.id && identity.presentation == 67);
    packet[40] &= ~0x20;
    assert(!np_presentation_time_packet_identity(packet, size, &identity));
    packet[40] |= 0x20;
    assert(!np_presentation_time_packet_identity(packet, size - 1, &identity));
    packet[40] |= 0x80;
    assert(!np_presentation_time_packet_identity(packet, size, &identity));
    free(packet); np_backend_surface_destroy(&fixture.root); np_backend_surface_destroy(&fixture.child); teardown(&fixture);
}
static void flat_remote_cancel_before_display_has_proven_discard(void)
{
    struct fixture fixture; struct np_remote_backend backend;
    initialize_publication(&fixture, &backend); initialize_flat(&fixture, NP_SURFACE_ROLE_DRAG_ICON); use_vm = false;
    uint32_t query = request_feedback(&fixture, &fixture.child);
    attach_scene(&fixture.child, 68, 68);
    assert(np_presentation_queue_last(&fixture.child, 68));
    np_presentation_flush(&fixture.server);
    assert(backend.jobs[0] && find_scene(fixture.server.presentation_time, 1, fixture.child.id, 68));
    np_presentation_time_discard_surface(&fixture.child); /* Retired pixels cannot be replayed later. */
    np_remote_cancel_scenes(&fixture.server, fixture.child.id);
    np_remote_flush_encoded(&fixture.server);
    expect_event(&fixture, 0, query, false, 0);
    assert(!displayed && !backend.jobs[0] && !backend.jobs[1] && !fixture.errors);
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    assert(holds == 1); /* Display outcome remains separate from source lifetime. */
    np_presentation_process_released(&fixture.server, fixture.child.id, 68);
    np_backend_surface_destroy(&fixture.root); np_backend_surface_destroy(&fixture.child);
    np_remote_finish_scenes(&fixture.server); teardown(&fixture);
}
static void flat_reconnect_replays_true_result_with_its_original_clock(void)
{
    struct fixture fixture; struct np_remote_backend backend;
    initialize_publication(&fixture, &backend); initialize_flat(&fixture, NP_SURFACE_ROLE_CURSOR); use_vm = true;
    int peer = initialize_socket();
    uint32_t query = request_feedback(&fixture, &fixture.child);
    attach_scene(&fixture.child, 69, 69);
    assert(np_presentation_queue_last(&fixture.child, 69));
    np_presentation_flush(&fixture.server);
    assert(!fixture.child.pending_frame && holds == 1 && vm_host.conn_fd < 0);
    struct np_display_scene *record = find_scene(fixture.server.presentation_time, 1, fixture.child.id, 69);
    assert(record && record->submitted);
    struct np_presentation_clock original = record->clock;
    np_presentation_time_reset(&fixture.server);
    fixture.now += 6 * MS; result(&fixture, 1, fixture.child.id, 69, 900 * SECOND + 5 * MS);
    assert(!fixture.event_count && record->terminal);
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS; clock_reply(&fixture, 900 * SECOND + 11 * MS);
    expect_event(&fixture, 0, query, true, 100 * SECOND + 5 * MS);
    assert(original.host == 900 * SECOND + MS);
    result(&fixture, 1, fixture.child.id, 69, 900 * SECOND + 5 * MS);
    assert(fixture.event_count == 1 && !fixture.errors && wl_list_empty(&fixture.server.presentation_time->scenes));
    np_presentation_process_released(&fixture.server, fixture.child.id, 69);
    np_backend_surface_destroy(&fixture.root); np_backend_surface_destroy(&fixture.child);
    np_host_finish(&vm_host); close(peer); teardown(&fixture);
}
static void reconnect_current_zero_defers_protected_replay_until_its_query_returns(bool flat)
{
    struct fixture fixture; struct np_remote_backend backend;
    initialize_publication(&fixture, &backend); use_vm = true;
    if (flat) initialize_flat(&fixture, NP_SURFACE_ROLE_CURSOR);
    struct np_surface *surface = flat ? &fixture.child : &fixture.root;
    int old_peer = initialize_socket();
    uint32_t query = request_feedback(&fixture, surface);
    attach_scene(surface, 70, 70);
    if (flat) assert(np_presentation_queue_last(surface, 70));
    else { surface->scene_dirty = true; surface->scene_presentation_id = 70; }
    np_presentation_flush(&fixture.server);
    assert(holds == 1 && vm_host.conn_fd < 0);
    struct np_display_scene *old = find_scene(fixture.server.presentation_time, 1, surface->id, 70);
    assert(old && old->submitted && wl_list_length(&old->feedbacks) == 1);
    np_presentation_time_reset(&fixture.server);
    /* Replay uses a fresh pixel-read hold. Old outcome ownership is independent. */
    np_presentation_process_released(&fixture.server, surface->id, 70);
    np_presentation_time_replay_surface(surface, surface->id, 71);
    if (flat) assert(np_presentation_queue_last(surface, 71));
    else { surface->scene_dirty = true; surface->scene_presentation_id = 71; }
    np_host_finish(&vm_host); close(old_peer); writes = 0;
    int peer = initialize_socket();
    np_presentation_time_clock_request(&fixture.server);
    fixture.now += 2 * MS; clock_reply(&fixture, 900 * SECOND + 5 * MS);
    np_presentation_flush(&fixture.server);
    /* Connected/calibrated replay must not display the queried current commit
     * untracked while the old attempt still owns its first-display query. */
    assert(!writes && !fixture.event_count && find_scene(fixture.server.presentation_time, 1, surface->id, 70) == old);
    assert(flat ? surface->pending_frame && holds == 1 : surface->scene_dirty && !holds);
    uint64_t session = np_presentation_time_session(&fixture.server);
    np_presentation_time_pause(&fixture.server, session, 1);
    np_presentation_time_drain(&fixture.server, session, 1);
    assert(!sent_count(&fixture, NP_GUEST_PRESENTATION_DRAINED));
    result(&fixture, 1, surface->id, 70, 0);
    assert(!fixture.event_count && !fixture.errors && !find_scene(fixture.server.presentation_time, 1, surface->id, 70));
    assert(sent_count(&fixture, NP_GUEST_PRESENTATION_RESULT_ACK) == 1 &&
        sent_count(&fixture, NP_GUEST_PRESENTATION_DRAINED) == 1);
    struct np_display_feedback *pending = wl_container_of(surface->presentation_feedbacks.next, pending, link);
    assert(pending->commit == 70 && pending->scene == 71 && pending->surface == surface);
    result(&fixture, 1, surface->id, 70, 0); /* Duplicate zero cannot consume the reattached query. */
    assert(!fixture.event_count && !wl_list_empty(&surface->presentation_feedbacks));
    np_presentation_time_resume(&fixture.server, session, 2);
    fixture.now += 2 * MS; clock_reply(&fixture, 900 * SECOND + 7 * MS);
    np_presentation_flush(&fixture.server);
    assert(writes == 2 && vm_host.conn_fd < 0 && holds == 1);
    assert(flat ? !surface->pending_frame : !surface->scene_dirty);
    unsigned char stream[200];
    size_t wire_size = flat ? 84 : 200;
    assert(read(peer, stream, sizeof(stream)) == (ssize_t)wire_size);
    struct np_presentation_identity identity;
    assert(np_presentation_time_packet_identity(stream + 12, wire_size - 12, &identity));
    assert(identity.epoch == 2 && identity.owner == surface->id && identity.presentation == 71);
    if (flat) assert(scene_packet_u32(stream + 12 + 40) == 0x60); /* Hybrid cursor must take Metal. */
    struct np_display_scene *replay = find_scene(fixture.server.presentation_time, 2, surface->id, 71);
    assert(replay && replay->submitted && wl_list_length(&replay->feedbacks) == 1);
    fixture.now += 3 * MS; result(&fixture, 2, surface->id, 71, 900 * SECOND + 10 * MS);
    expect_event(&fixture, 0, query, true, 100 * SECOND + 10 * MS);
    assert(!fixture.errors && wl_list_empty(&fixture.server.presentation_time->scenes));
    np_presentation_process_released(&fixture.server, surface->id, 71);
    assert(!holds);
    np_backend_surface_destroy(&fixture.root); np_backend_surface_destroy(&fixture.child);
    np_host_finish(&vm_host); close(peer); teardown(&fixture);
}
int main(void)
{
    vm_admission_retains_source_after_optional_write_failure(true);
    vm_admission_retains_source_after_optional_write_failure(false);
    vm_flat_frame_admission_retains_source_after_optional_write_failure();
    remote_backpressure_does_not_leave_a_pause_ghost_or_old_epoch_owner();
    flat_pre_role_supersession_and_destroy_have_truthful_outcomes();
    flat_remote_rejection_pause_resume_and_actual_time_use_new_epoch();
    flat_clock_context_follows_all_variable_frame_fields();
    flat_remote_cancel_before_display_has_proven_discard();
    flat_reconnect_replays_true_result_with_its_original_clock();
    reconnect_current_zero_defers_protected_replay_until_its_query_returns(false);
    reconnect_current_zero_defers_protected_replay_until_its_query_returns(true);
    puts("Scene/frame admission, auxiliary role lifetime, real clock context and pause/drain: 11 groups PASS");
    return 0;
}
