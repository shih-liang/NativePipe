/* Exercise real wl_surface.commit snapshots and wp_presentation resources.
 * Only buffer publication is controlled; feedback ownership/events are real. */
#define main presentation_time_fixture_checks_main
#define np_window_event_send presentation_fixture_window_event_send
#include "presentation_time_test.c"
#undef np_window_event_send
#undef main
bool np_window_event_send(struct np_server *server, uint8_t opcode,
    const uint32_t *values, size_t count);
#include "../surface_commit.c"
#include "../region.c"

static unsigned queued, refreshed, unmapped, released, rebound;
static uint32_t released_id, rebound_from, rebound_to;

bool np_window_event_send(struct np_server *server, uint8_t opcode,
    const uint32_t *values, size_t count)
{
    assert(server == &active->server);
    assert(opcode == NP_GUEST_SURFACE_UNMAPPED && count == 1);
    assert(values[0] == active->child.id);
    unmapped++;
    return true;
}

bool np_trace_enabled(void) { return false; }
struct np_gpu_buffer *np_gpu_buffer_get(struct wl_resource *buffer)
{
    return buffer ? wl_resource_get_user_data(buffer) : NULL;
}
void np_gpu_buffer_retain(struct np_gpu_buffer *buffer) { (void)buffer; }
void np_gpu_buffer_drop(struct np_gpu_buffer *buffer) { (void)buffer; }
bool np_gpu_buffer_is_busy(struct np_gpu_buffer *buffer) { (void)buffer; return false; }
enum np_gpu_read_result np_gpu_buffer_render_status(struct np_gpu_buffer *buffer, int *fd)
{
    assert(buffer);
    *fd = -1;
    return NP_GPU_READ_READY;
}
bool np_syncobj_has_pending(struct np_surface *surface) { (void)surface; return false; }
bool np_syncobj_take_commit(struct np_surface *surface, bool set, struct wl_resource *buffer,
    struct np_sync_point **acquire, struct np_sync_point **release)
{
    (void)surface; (void)set; (void)buffer;
    *acquire = *release = NULL;
    return true;
}
bool np_sync_point_ready(struct np_sync_point *point) { assert(!point); return true; }
int np_sync_point_wait_fd(struct np_sync_point *point) { assert(!point); return -1; }
void np_sync_point_destroy(struct np_sync_point *point) { assert(!point); }
void np_sync_point_signal(struct np_sync_point *point) { assert(!point); }

enum np_scale_error np_scale_resolve_state(uint32_t width, uint32_t height,
    int32_t scale, int32_t transform, const struct np_viewport_state *viewport,
    struct np_surface_mapping *mapping)
{
    assert(width == 4 && height == 4 && scale == 1 && transform == 0);
    assert(!viewport->source_set && !viewport->destination_set);
    *mapping = (struct np_surface_mapping){.source_width_pixels = width,
        .source_height_pixels = height, .logical_width = width, .logical_height = height};
    return NP_SCALE_OK;
}
bool np_scale_damage_to_buffer(uint32_t width, uint32_t height, int32_t scale,
    int32_t transform, const struct np_viewport_state *viewport,
    const struct np_box *damage, struct np_box *converted)
{
    (void)width; (void)height; (void)scale; (void)transform; (void)viewport;
    *converted = *damage;
    return true;
}
void np_shm_texture_prepare_commit(struct np_surface *surface, uint32_t width,
    uint32_t height, bool mapping_changed)
{
    (void)surface; (void)width; (void)height; (void)mapping_changed;
}
void np_shm_texture_note_damage(struct np_surface *surface, const struct np_box *damage)
{
    (void)surface; (void)damage;
}
void np_scene_note_damage(struct np_surface *surface, const struct np_box *damage, bool full)
{
    (void)surface; (void)damage; (void)full;
}
bool np_scene_update_changes_structure(const struct np_surface_update *update,
    uint32_t old_width, uint32_t old_height, uint32_t width, uint32_t height, bool scaled)
{
    (void)update; (void)old_width; (void)old_height; (void)width; (void)height; (void)scaled;
    return false;
}
void np_scene_presented(struct np_surface *surface, uint32_t id)
{
    assert(surface == &active->child && id);
    released++;
    released_id = id;
}
uint32_t np_presentation_next_id(struct np_server *server)
{
    assert(server == &active->server);
    return ++server->next_presentation_id;
}
bool np_presentation_has_unbound_callbacks(struct np_surface *surface)
{
    (void)surface; return false;
}
bool np_presentation_bind_callbacks(struct np_surface *surface, uint32_t id)
{
    (void)surface; (void)id; return false;
}
void np_presentation_rebind_callbacks(struct np_surface *surface, uint32_t from, uint32_t to)
{
    assert(surface == &active->child && from && to);
    rebound++; rebound_from = from; rebound_to = to;
}
void np_presentation_request_refresh(struct np_surface *surface, uint32_t id)
{
    assert(surface == &active->child && id);
    refreshed++;
}
bool np_presentation_queue_last(struct np_surface *surface, uint32_t id)
{
    assert(surface == &active->child && id);
    if (!surface->has_published || !surface->last_resource_id) return false;
    free(surface->pending_frame);
    surface->pending_frame = malloc(1);
    assert(surface->pending_frame);
    surface->pending_frame_size = 1;
    surface->pending_presentation_id = id;
    queued++;
    return true;
}
void np_presentation_clear_scene_wait(struct np_surface *surface) { (void)surface; }
void np_presentation_flush(struct np_server *server) { (void)server; }
void np_backend_session_sync(struct np_server *server) { (void)server; }
bool np_backend_refresh_current_shm(struct np_surface *surface, uint32_t id,
    const struct np_box *damage)
{
    (void)surface; (void)id; (void)damage; assert(false); return false;
}
void np_backend_publish_buffer(struct np_surface *surface, struct wl_resource *buffer,
    struct np_gpu_buffer *gpu, enum np_buffer_commit_kind kind, uint32_t id,
    struct np_sync_point *release, const struct np_box *damage)
{
    assert(surface == &active->child && !release);
    (void)damage;
    if (kind == NP_BUFFER_DETACH) {
        surface->current_buffer = NULL;
        surface->current_gpu = NULL;
        surface->has_published = false;
        np_presentation_request_refresh(surface, id);
        return;
    }
    assert(kind == NP_BUFFER_ATTACH && buffer && gpu);
    surface->current_buffer = buffer;
    surface->current_gpu = gpu;
    surface->has_published = true;
    surface->last_resource_id = gpu->resource_id;
    surface->last_width = gpu->width;
    surface->last_height = gpu->height;
    assert(np_presentation_queue_last(surface, id));
}
void np_xdg_clear_configures(struct np_surface *surface) { (void)surface; assert(false); }
void np_xdg_send_initial_role_configure(struct np_surface *surface) { (void)surface; assert(false); }
void np_xdg_finish_toplevel_configure(struct np_surface *surface, uint32_t serial)
{
    (void)surface; (void)serial; assert(false);
}
void np_xdg_apply_popup_geometry(struct np_surface *surface, int32_t x, int32_t y,
    int32_t width, int32_t height)
{
    (void)surface; (void)x; (void)y; (void)width; (void)height; assert(false);
}
void np_xdg_apply_size_constraints(struct np_surface *surface, int32_t min_width,
    int32_t min_height, int32_t max_width, int32_t max_height)
{
    (void)surface; (void)min_width; (void)min_height; (void)max_width; (void)max_height;
    assert(false);
}

static void flat_setup(struct fixture *fixture, enum np_surface_role role)
{
    setup(fixture, true);
    fixture->unsupported_child = true;
    fixture->child.resource = fixture->child_resource;
    fixture->child.scale = fixture->child.pending_scale = 1;
    fixture->child.role = role;
    wl_list_init(&fixture->child.children);
    wl_list_init(&fixture->child.sibling_link);
    wl_list_init(&fixture->child.pending_stack_ops);
    wl_list_init(&fixture->child.pending_frame_callbacks);
    wl_list_init(&fixture->child.blocked_updates);
    wl_list_init(&fixture->child.synchronized_updates);
    wl_list_init(&fixture->child.scene_presentations);
    wl_list_init(&fixture->child.xdg_configures);
    queued = refreshed = unmapped = released = rebound = 0;
    released_id = rebound_from = rebound_to = 0;
}

static void flat_teardown(struct fixture *fixture)
{
    assert(wl_list_empty(&fixture->child.blocked_updates));
    assert(wl_list_empty(&fixture->child.synchronized_updates));
    free(fixture->child.pending_frame);
    fixture->child.pending_frame = NULL;
    teardown(fixture);
}

static struct wl_resource *make_buffer(struct fixture *fixture, struct np_gpu_buffer *gpu)
{
    *gpu = (struct np_gpu_buffer){.resource_id = 42, .width = 4, .height = 4,
        .stride = 16, .format = WL_SHM_FORMAT_ARGB8888};
    struct wl_resource *buffer = wl_resource_create(fixture->client,
        &wl_buffer_interface, 1, fixture->next_id++);
    assert(buffer);
    wl_resource_set_user_data(buffer, gpu);
    return buffer;
}

static uint32_t commit_query(struct fixture *fixture, struct wl_resource *buffer, bool attach)
{
    uint32_t query = request_feedback(fixture, &fixture->child);
    if (attach) {
        fixture->child.pending_buffer = buffer;
        fixture->child.pending_buffer_set = true;
    }
    np_surface_commit(fixture->client, fixture->child_resource);
    assert(!fixture->errors);
    return query;
}

static bool prepare_flat_packet(struct fixture *fixture, uint32_t id,
    struct np_window_message *message)
{
    struct np_window_frame frame = {.resource_id = 42, .width = 4, .height = 4,
        .bytes_per_row = 16, .scale = 1, .format = NP_WINDOW_BGRA8888,
        .source = NP_WINDOW_FRAME_GPU, .presentation_id = id,
        .has_gpu_source = true, .gpu_source_id = 42, .has_presentation_context = true};
    np_window_message_init(message, NP_WINDOW_GUEST_TO_HOST, NP_GUEST_COMMITTED);
    np_window_put_u32(message, fixture->child.id);
    np_window_put_frame(message, &frame);
    assert(message->ok);
    return np_presentation_time_prepare_scene(&fixture->server,
        fixture->child.id, id, message->data, message->len);
}

static void submit_flat_packet(struct fixture *fixture, uint32_t id)
{
    struct np_window_message message;
    assert(prepare_flat_packet(fixture, id, &message));
    struct np_presentation_identity identity;
    assert(np_presentation_time_packet_identity(message.data, message.len, &identity));
    assert(identity.owner == fixture->child.id && identity.presentation == id);
    assert(identity.session == np_presentation_time_session(&fixture->server));
    assert(identity.epoch == 1 && identity.sent == fixture->now);
    np_presentation_time_submitted(&fixture->server, message.data, message.len);
    np_window_message_clear(&message);
}

static void clear_admitted_mailbox(struct np_surface *surface)
{
    free(surface->pending_frame);
    surface->pending_frame = NULL;
    surface->pending_frame_size = surface->pending_presentation_id = 0;
}

static void pre_role_and_retained_commits_keep_feedback(void)
{
    struct fixture fixture;
    flat_setup(&fixture, NP_SURFACE_ROLE_NONE);
    struct np_gpu_buffer gpu;
    struct wl_resource *buffer = make_buffer(&fixture, &gpu);
    uint32_t attached = commit_query(&fixture, buffer, true);
    struct np_display_feedback *feedback = wl_resource_get_user_data(
        wl_client_get_object(fixture.client, attached));
    assert(feedback->surface == &fixture.child && feedback->commit);
    assert(feedback->scene == fixture.child.pending_presentation_id);
    assert(feedback->owner == fixture.child.id && queued == 1);
    assert(!fixture.event_count && wl_list_empty(&fixture.server.presentation_time->scenes));
    fixture.now += 16 * SECOND;
    tick(fixture.server.presentation_time);
    assert(!fixture.errors && !fixture.event_count); /* Role wait is not submitted timeout. */
    uint32_t retained = commit_query(&fixture, NULL, false);
    assert(queued == 2 && fixture.event_count == 1);
    expect_event(&fixture, 0, attached, false, 0); /* Only now superseded. */
    feedback = wl_resource_get_user_data(wl_client_get_object(fixture.client, retained));
    assert(feedback->surface == &fixture.child && feedback->owner == fixture.child.id);
    assert(feedback->scene == fixture.child.pending_presentation_id);
    assert(!fixture.errors && wl_list_empty(&fixture.server.presentation_time->scenes));
    flat_teardown(&fixture);
}

static void empty_commits_have_no_display_content(void)
{
    struct fixture fixture;
    flat_setup(&fixture, NP_SURFACE_ROLE_NONE);
    uint32_t empty = commit_query(&fixture, NULL, false);
    assert(fixture.event_count == 1 && !fixture.child.has_published && !queued && !unmapped);
    expect_event(&fixture, 0, empty, false, 0);
    uint32_t detached = commit_query(&fixture, NULL, true);
    assert(fixture.event_count == 2 && !unmapped && !fixture.errors);
    expect_event(&fixture, 1, detached, false, 0);
    flat_teardown(&fixture);
}

static void retained_import_is_not_empty_publication_state(void)
{
    struct fixture fixture;
    flat_setup(&fixture, NP_SURFACE_ROLE_NONE);
    struct np_gpu_buffer gpu;
    struct wl_resource *buffer = make_buffer(&fixture, &gpu);
    fixture.child.current_buffer = buffer;
    fixture.child.current_gpu = &gpu;
    fixture.child.last_resource_id = gpu.resource_id;
    fixture.child.last_width = fixture.child.last_height = 4;
    /* A retained import is real current content even before host publication. */
    assert(!fixture.child.has_published);
    uint32_t query = commit_query(&fixture, NULL, false);
    struct np_display_feedback *feedback = wl_resource_get_user_data(
        wl_client_get_object(fixture.client, query));
    assert(feedback && feedback->scene && feedback->owner == fixture.child.id);
    assert(!fixture.event_count && !fixture.errors && !queued && refreshed == 1);
    flat_teardown(&fixture);
}

static void detach_retires_pending_but_preserves_submitted(enum np_surface_role role)
{
    struct fixture fixture;
    flat_setup(&fixture, role);
    struct np_gpu_buffer gpu;
    struct wl_resource *buffer = make_buffer(&fixture, &gpu);
    uint32_t submitted_query = commit_query(&fixture, buffer, true);
    uint32_t submitted_id = fixture.child.pending_presentation_id;
    submit_flat_packet(&fixture, submitted_id);
    /* Successful backend admission transfers the envelope out of the mailbox. */
    clear_admitted_mailbox(&fixture.child);
    uint32_t pending_query = commit_query(&fixture, NULL, false);
    uint32_t pending_id = fixture.child.pending_presentation_id;
    assert(!fixture.event_count && pending_id != submitted_id);
    uint32_t detached_query = commit_query(&fixture, NULL, true);
    uint32_t detached_id = fixture.server.next_presentation_id;
    assert(!fixture.child.pending_frame && !fixture.child.pending_presentation_id);
    assert(!fixture.child.pending_frame_size && !fixture.child.current_gpu && !fixture.child.current_buffer);
    assert(!fixture.child.has_published && unmapped == 1 && released == 1 && rebound == 1);
    assert(released_id == pending_id && rebound_from == pending_id && rebound_to == detached_id);
    assert(fixture.event_count == 2);
    expect_event(&fixture, 0, pending_query, false, 0);
    expect_event(&fixture, 1, detached_query, false, 0);
    assert(wl_client_get_object(fixture.client, submitted_query));
    fixture.now = 100 * SECOND + 8 * MS;
    result(&fixture, 1, fixture.child.id, submitted_id, 900 * SECOND + 6 * MS);
    assert(fixture.event_count == 3 && !fixture.errors);
    expect_event(&fixture, 2, submitted_query, true, 100 * SECOND + 6 * MS);
    result(&fixture, 1, fixture.child.id, submitted_id, 900 * SECOND + 6 * MS);
    assert(fixture.event_count == 3);
    uint32_t repeated = commit_query(&fixture, NULL, true);
    expect_event(&fixture, 3, repeated, false, 0);
    assert(unmapped == 1 && released == 1 && rebound == 1);
    flat_teardown(&fixture);
}

static void current_zero_rebinds_pending_flat_retry_until_actual_display(void)
{
    struct fixture fixture;
    flat_setup(&fixture, NP_SURFACE_ROLE_CURSOR);
    struct np_gpu_buffer gpu;
    struct wl_resource *buffer = make_buffer(&fixture, &gpu);
    uint32_t query = commit_query(&fixture, buffer, true);
    uint32_t original = fixture.child.pending_presentation_id;
    submit_flat_packet(&fixture, original);
    clear_admitted_mailbox(&fixture.child);
    /* Repainting the current cursor is not a new wl_surface.commit. */
    uint32_t retry = np_presentation_next_id(&fixture.server);
    assert(np_presentation_queue_last(&fixture.child, retry));
    struct np_window_message blocked;
    assert(!prepare_flat_packet(&fixture, retry, &blocked));
    np_window_message_clear(&blocked);
    assert(!fixture.event_count && !fixture.errors);
    result(&fixture, 1, fixture.child.id, original, 0);
    struct np_display_feedback *feedback = wl_resource_get_user_data(
        wl_client_get_object(fixture.client, query));
    assert(feedback && feedback->surface == &fixture.child && feedback->commit == original);
    assert(feedback->surface_id == fixture.child.id && !feedback->retired);
    assert(feedback->scene == retry && feedback->owner == fixture.child.id);
    assert(!fixture.event_count && !fixture.errors && !unmapped && !released);
    assert(fixture.child.pending_frame && fixture.child.pending_presentation_id == retry);
    assert(wl_list_empty(&fixture.server.presentation_time->scenes));
    submit_flat_packet(&fixture, retry);
    clear_admitted_mailbox(&fixture.child);
    fixture.now = 100 * SECOND + 8 * MS;
    result(&fixture, 1, fixture.child.id, retry, 900 * SECOND + 6 * MS);
    assert(fixture.event_count == 1 && !fixture.errors && !unmapped && !released);
    expect_event(&fixture, 0, query, true, 100 * SECOND + 6 * MS);
    result(&fixture, 1, fixture.child.id, original, 0);
    result(&fixture, 1, fixture.child.id, original, 900 * SECOND + 5 * MS);
    assert(fixture.event_count == 1 && wl_list_empty(&fixture.server.presentation_time->scenes));
    flat_teardown(&fixture);
}

int main(void)
{
    pre_role_and_retained_commits_keep_feedback();
    empty_commits_have_no_display_content();
    retained_import_is_not_empty_publication_state();
    detach_retires_pending_but_preserves_submitted(NP_SURFACE_ROLE_CURSOR);
    detach_retires_pending_but_preserves_submitted(NP_SURFACE_ROLE_DRAG_ICON);
    current_zero_rebinds_pending_flat_retry_until_actual_display();
    puts("PASS: flat commit role wait, retained contents, empty/detach and submitted feedback isolation");
    return 0;
}
