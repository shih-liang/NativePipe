/* Actual display feedback is independent of FIFO latches and pixel-read release. */
#include "presentation_time.h"
#include "presentation_clock.h"
#include "backend.h"
#include "compositor_internal.h"
#include "presentation-time-server-protocol.h"
#include "scale.h"
#include "scene.h"
#include "window_events.h"
#include "windowwire.h"
#include "user_text.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CLOCK_RETRY_NS NP_PRESENTATION_CLOCK_REMOTE_MAX_RTT_NS
#define CLOCK_RECOVERY_NS UINT64_C(2000000000)
#define CLOCK_TIMEOUT_NS UINT64_C(3000000000)
#define RESULT_TIMEOUT_NS UINT64_C(15000000000)
#define MAX_SCENES 4096u

struct np_presentation_time {
    struct np_server *server;
    struct wl_global *global;
    struct wl_event_source *timer;
    struct np_presentation_clock clock;
    struct np_presentation_clock disconnected_clock;
    struct wl_list scenes;
    uint64_t session, epoch, sent, calibration_started;
    uint32_t next_token, token, pause_token, drain_token, resume_token, completed_resume, last_barrier;
    bool capable, paused, pause_reached, reconnecting, history_unverified, faulted;
};
struct np_display_feedback {
    struct wl_list link;
    struct wl_resource *resource;
    struct np_surface *surface; /* Only unsubmitted feedback is surface-owned. */
    uint32_t commit, scene, owner, surface_id;
    bool retired;
};
struct np_display_scene {
    struct wl_list link, feedbacks;
    struct np_presentation_time *state;
    uint64_t epoch, sent, created, host, mapping_ceiling;
    struct np_presentation_clock clock;
    uint32_t owner, scene, output;
    bool submitted, terminal;
};
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}
static bool has_list(struct np_surface *surface)
{
    return surface && surface->presentation_feedbacks.next;
}
static void feedback_destroy(struct wl_resource *resource)
{
    struct np_display_feedback *feedback = wl_resource_get_user_data(resource);
    wl_list_remove(&feedback->link);
    free(feedback);
}
static void discard(struct np_display_feedback *feedback)
{
    wp_presentation_feedback_send_discarded(feedback->resource);
    wl_resource_destroy(feedback->resource);
}
static void feedback_fail(struct np_display_feedback *feedback, const char *reason)
{
    wl_client_post_implementation_error(wl_resource_get_client(feedback->resource), "%s", reason);
    wl_resource_destroy(feedback->resource);
}
static void scene_destroy(struct np_display_scene *scene)
{
    struct np_display_feedback *feedback, *tmp;
    wl_list_for_each_safe(feedback, tmp, &scene->feedbacks, link)
        wl_resource_destroy(feedback->resource);
    wl_list_remove(&scene->link);
    free(scene);
}
static void scene_fail(struct np_display_scene *scene)
{
    /* An unavailable outcome is not proof that this update was never shown. */
    struct np_display_feedback *feedback, *tmp;
    wl_list_for_each_safe(feedback, tmp, &scene->feedbacks, link)
        feedback_fail(feedback, "NativePipe could not recover the actual presentation outcome");
    scene_destroy(scene);
}
static struct np_display_scene *find_scene(struct np_presentation_time *state,
    uint64_t epoch, uint32_t owner, uint32_t id)
{
    struct np_display_scene *scene;
    wl_list_for_each(scene, &state->scenes, link)
        if (scene->epoch == epoch && scene->owner == owner && scene->scene == id) return scene;
    return NULL;
}
static void result_ack(struct np_presentation_time *state, uint64_t epoch,
    uint32_t owner, uint32_t scene)
{
    uint32_t fields[] = { state->session, state->session >> 32, epoch, epoch >> 32, owner, scene };
    np_window_event_send(state->server, NP_GUEST_PRESENTATION_RESULT_ACK, fields, 6);
}
static void send_fence(struct np_presentation_time *state, uint8_t opcode, uint32_t token)
{
    uint32_t fields[] = { state->session, state->session >> 32, token };
    np_window_event_send(state->server, opcode, fields, 3);
}
static void finish_drain(struct np_presentation_time *state)
{
    if (state->drain_token && wl_list_empty(&state->scenes)) {
        send_fence(state, NP_GUEST_PRESENTATION_DRAINED, state->drain_token);
        state->drain_token = 0;
    }
}
static void finish_pause(struct np_presentation_time *state)
{
    if (state->paused && state->pause_token && !state->pause_reached &&
        np_backend_display_boundary_ready(state->server)) {
        send_fence(state, NP_GUEST_PRESENTATION_PAUSE_REACHED, state->pause_token);
        state->pause_reached = true;
    }
}
static struct np_surface *feedback_surface(struct np_presentation_time *state, uint32_t id)
{
    struct np_surface *surface;
    wl_list_for_each(surface, &state->server->surfaces, link)
        if (surface->id == id) return surface;
    return NULL;
}
static void defer_current_feedback(struct np_display_feedback *feedback, struct np_display_scene *scene)
{
    struct np_surface *surface = feedback_surface(scene->state, feedback->surface_id);
    if (feedback->retired || !surface) { discard(feedback); return; }
    /* Zero proves this attempt was not shown, not that the same current
     * wl_surface commit can never be displayed by a protected replay. */
    struct np_surface *root = np_scene_root(surface);
    uint32_t pending = root ? root->scene_presentation_id : surface->pending_presentation_id;
    feedback->owner = root ? root->id : surface->id;
    if (pending) feedback->scene = pending;
    feedback->surface = surface;
    wl_list_remove(&feedback->link);
    wl_list_insert(surface->presentation_feedbacks.prev, &feedback->link);
}
static bool flush_result(struct np_display_scene *scene)
{
    if (!scene->terminal) return false;
    /* Replay can precede the first clock reply. Verify continuity before using
     * old anchors; elapsed guest time alone cannot reveal a paused interval. */
    if (scene->host && scene->state->history_unverified) return false;
    if (scene->host && scene->mapping_ceiling && scene->host > scene->mapping_ceiling) {
        struct np_presentation_time *state = scene->state;
        uint64_t epoch = scene->epoch;
        uint32_t owner = scene->owner, id = scene->scene;
        scene_fail(scene);
        result_ack(state, epoch, owner, id);
        finish_drain(state);
        return true;
    }
    uint64_t guest = 0;
    if (scene->host && !wl_list_empty(&scene->feedbacks) &&
        !np_presentation_clock_convert(&scene->clock, scene->host, now_ns(), &guest)) return false;
    struct np_display_feedback *feedback, *tmp;
    wl_list_for_each_safe(feedback, tmp, &scene->feedbacks, link) {
        if (!scene->host) { defer_current_feedback(feedback, scene); continue; }
        np_scale_presentation_output_for_server(scene->state->server, feedback->resource, scene->output);
        uint64_t seconds = guest / 1000000000u;
        /* v1 variable refresh prediction is zero. No hardware clock, MSC,
         * completion or zero-copy claim follows from the software mapping. */
        wp_presentation_feedback_send_presented(feedback->resource,
            seconds >> 32, seconds, guest % 1000000000u, 0, 0, 0, 0);
        wl_resource_destroy(feedback->resource);
    }
    struct np_presentation_time *state = scene->state;
    result_ack(state, scene->epoch, scene->owner, scene->scene);
    scene_destroy(scene);
    finish_drain(state);
    return true;
}
static void manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}
static void manager_feedback(struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *surface_resource, uint32_t id)
{
    (void)resource;
    struct np_surface *surface = wl_resource_get_user_data(surface_resource);
    if (!surface) return;
    struct np_display_feedback *feedback = calloc(1, sizeof(*feedback));
    if (!feedback) { wl_client_post_no_memory(client); return; }
    feedback->resource = wl_resource_create(client, &wp_presentation_feedback_interface, 1, id);
    if (!feedback->resource) { free(feedback); wl_client_post_no_memory(client); return; }
    feedback->surface = surface;
    feedback->surface_id = surface->id;
    wl_resource_set_implementation(feedback->resource, NULL, feedback, feedback_destroy);
    wl_list_insert(surface->presentation_feedbacks.prev, &feedback->link);
    np_presentation_time_clock_request(surface->server);
}
static const struct wp_presentation_interface implementation = {
    .destroy = manager_destroy, .feedback = manager_feedback,
};
static void bind_presentation(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)version;
    struct wl_resource *resource = wl_resource_create(client, &wp_presentation_interface, 1, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &implementation, data, NULL);
    wp_presentation_send_clock_id(resource, CLOCK_MONOTONIC);
}
void np_presentation_time_clock_request(struct np_server *server)
{
    struct np_presentation_time *state = server->presentation_time;
    if (!state || !server->host_session_ready ||
        (state->paused && !state->resume_token)) return;
    uint64_t now = now_ns();
    /* A timeout keeps public feedback unavailable, but a recovered transport
     * can qualify it later. Avoid busy retry while preserving the same nonce
     * and clock epoch; only a valid reply clears faulted below. */
    if (state->faulted && now - state->sent < CLOCK_RECOVERY_NS) return;
    if (state->token && now - state->sent < CLOCK_RETRY_NS) return;
    if (state->clock.valid && now - state->sent < CLOCK_RETRY_NS) return;
    uint32_t token = ++state->next_token;
    if (!token) token = ++state->next_token;
    state->token = token;
    state->sent = now;
    if (!state->calibration_started) state->calibration_started = now;
    uint32_t fields[] = { token, state->session, state->session >> 32, state->epoch, state->epoch >> 32 };
    np_window_event_send(server, NP_GUEST_PRESENTATION_CLOCK_REQUESTED, fields, 5);
}
static int tick(void *data)
{
    struct np_presentation_time *state = data;
    uint64_t now = now_ns();
    struct np_display_scene *scene, *tmp;
    wl_list_for_each_safe(scene, tmp, &state->scenes, link) {
        if (scene->terminal && flush_result(scene)) continue;
        if (now - scene->created >= RESULT_TIMEOUT_NS) scene_fail(scene);
    }
    if (!state->clock.valid && state->calibration_started &&
        now - state->calibration_started >= CLOCK_TIMEOUT_NS) {
        if (!state->faulted)
            fprintf(stderr, "[wayland] presentation clock calibration timed out\n");
        state->faulted = true;
        /* Ordinary rendering needs no public clock promise. A failed explicit
         * resume still keeps its gate, so the host can report/retry the failure. */
        if (state->reconnecting && !state->resume_token) {
            state->reconnecting = false;
            state->paused = false;
        }
    }
    finish_pause(state);
    finish_drain(state);
    np_presentation_time_clock_request(state->server);
    wl_event_source_timer_update(state->timer, 250);
    return 0;
}
void np_presentation_time_init(struct np_server *server)
{
    if (server->presentation_time) return;
    struct np_presentation_time *state = calloc(1, sizeof(*state));
    if (!state) return;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t count;
    do { count = fd < 0 ? -1 : read(fd, &state->session, sizeof(state->session)); } while (count < 0 && errno == EINTR);
    if (fd >= 0) close(fd);
    if (count != (ssize_t)sizeof(state->session) || !state->session) {
        fprintf(stderr, "[wayland] could not create presentation session identity\n");
        free(state); return;
    }
    state->server = server;
    state->clock.maximum_rtt_ns = np_backend_presentation_clock_window();
    state->epoch = 1;
    wl_list_init(&state->scenes);
    state->timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->display), tick, state);
    if (!state->timer) { free(state); return; }
    server->presentation_time = state;
    wl_event_source_timer_update(state->timer, 250);
}
void np_presentation_time_advertise(struct np_server *server)
{
    np_presentation_time_init(server);
    struct np_presentation_time *state = server->presentation_time;
    if (!state || state->global) return;
    state->global = wl_global_create(server->display, &wp_presentation_interface, 1, state, bind_presentation);
}
void np_presentation_time_enable(struct np_server *server, bool capable)
{
    np_presentation_time_init(server);
    if (server->presentation_time) server->presentation_time->capable = capable;
}
uint64_t np_presentation_time_session(struct np_server *server)
{
    return server->presentation_time ? server->presentation_time->session : 0;
}
uint64_t np_presentation_time_epoch(struct np_server *server)
{
    return server->presentation_time ? server->presentation_time->epoch : 0;
}
bool np_presentation_time_emission_allowed(struct np_server *server)
{
    struct np_presentation_time *state = server->presentation_time;
    return !state || (!state->paused && !state->resume_token && !state->reconnecting);
}
bool np_presentation_time_has_pending(struct np_surface *surface)
{
    if (!has_list(surface)) return false;
    struct np_display_feedback *feedback;
    wl_list_for_each(feedback, &surface->presentation_feedbacks, link)
        if (!feedback->commit) return true;
    return false;
}
void np_presentation_time_commit(struct np_surface *surface, uint32_t commit)
{
    if (!has_list(surface)) return;
    struct np_display_feedback *feedback;
    wl_list_for_each(feedback, &surface->presentation_feedbacks, link)
        if (!feedback->commit) feedback->commit = commit;
}
static void retire_submitted_feedback(struct np_surface *surface, uint32_t commit, bool matching)
{
    struct np_presentation_time *state = surface->server->presentation_time;
    if (!state) return;
    struct np_display_scene *scene;
    wl_list_for_each(scene, &state->scenes, link) {
        struct np_display_feedback *feedback;
        wl_list_for_each(feedback, &scene->feedbacks, link)
            if (feedback->surface_id == surface->id &&
                (matching ? feedback->commit == commit : feedback->commit != commit))
                feedback->retired = true;
    }
}
void np_presentation_time_apply(struct np_surface *surface, uint32_t commit)
{
    if (!has_list(surface) || !commit) return;
    retire_submitted_feedback(surface, commit, false);
    struct np_display_feedback *feedback, *tmp;
    wl_list_for_each_safe(feedback, tmp, &surface->presentation_feedbacks, link)
        if (feedback->scene && feedback->commit != commit) discard(feedback);
}
void np_presentation_time_scene(struct np_surface *surface, uint32_t commit, uint32_t id)
{
    if (!has_list(surface)) return;
    struct np_surface *root = np_scene_root(surface);
    struct np_display_feedback *feedback;
    wl_list_for_each(feedback, &surface->presentation_feedbacks, link) {
        if (feedback->commit != commit) continue;
        feedback->scene = id;
        feedback->owner = root ? root->id : surface->id;
    }
}
void np_presentation_time_replay_surface(struct np_surface *surface, uint32_t owner, uint32_t id)
{
    if (!has_list(surface)) return;
    struct np_display_feedback *feedback;
    wl_list_for_each(feedback, &surface->presentation_feedbacks, link) {
        if (!feedback->scene) continue; /* Pending/cached commits are not current. */
        feedback->owner = owner;
        feedback->scene = id;
    }
}
static void put_u64(unsigned char *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = value >> (i * 8);
}
static uint64_t scene_packet_u64(const unsigned char *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static uint32_t scene_packet_u32(const unsigned char *p)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)p[i] << (i * 8);
    return value;
}
static unsigned char *packet_context(const void *packet, size_t size,
    uint32_t *owner, uint32_t *id)
{
    const unsigned char *bytes = packet;
    if (!bytes) return NULL;
    if (size >= 100 && !memcmp(bytes, "NPSN", 4)) {
        uint32_t count = scene_packet_u32(bytes + 48);
        if (bytes[4] != 4 || bytes[5] || bytes[6] != 100 || bytes[7] ||
            scene_packet_u32(bytes + 8) != size || !count || count > 128 ||
            size != 100 + (size_t)count * 88) return NULL;
        *owner = scene_packet_u32(bytes + 12); *id = scene_packet_u32(bytes + 16);
        return (unsigned char *)bytes + 76;
    }
    if (size < 72 || memcmp(bytes, "NPW2", 4) || bytes[4] != NP_WINDOW_GUEST_TO_HOST ||
        bytes[5] != NP_GUEST_COMMITTED || bytes[6] || bytes[7]) return NULL;
    uint32_t flags = scene_packet_u32(bytes + 40), count = scene_packet_u32(bytes + 44);
    if (!(flags & (1u << 5)) || flags & ~0x7fu || count > NP_WINDOW_MAX_COLLECTION) return NULL;
    size_t offset = 48, end = size - 24;
    if (flags & 1u) offset += 32;
    if (flags & 2u) offset += 8;
    if (flags & 4u) offset += 16;
    if (offset > end) return NULL;
    if (flags & 8u) {
        if (end - offset < 4) return NULL;
        uint32_t length = scene_packet_u32(bytes + offset);
        offset += 4;
        if (length > NP_WINDOW_MAX_FIELD || length > end - offset) return NULL;
        offset += length;
    }
    if (flags & 16u) offset += 4;
    if (offset > end || (size_t)count * 16 != end - offset) return NULL;
    *owner = scene_packet_u32(bytes + 8); *id = scene_packet_u32(bytes + 36);
    return (unsigned char *)bytes + end;
}
bool np_presentation_time_packet_identity(const void *packet, size_t size,
    struct np_presentation_identity *identity)
{
    if (!identity) return false;
    unsigned char *context = packet_context(packet, size, &identity->owner, &identity->presentation);
    if (!context) return false;
    identity->session = scene_packet_u64(context);
    identity->epoch = scene_packet_u64(context + 8);
    identity->sent = scene_packet_u64(context + 16);
    return identity->session && identity->epoch && identity->sent && identity->owner && identity->presentation;
}
static bool packet_samples_surface(const void *packet, size_t size, uint32_t surface)
{
    const unsigned char *bytes = packet;
    if (!memcmp(bytes, "NPW2", 4)) return scene_packet_u32(bytes + 8) == surface;
    uint32_t count = scene_packet_u32(bytes + 48);
    for (uint32_t i = 0; i < count && 100 + (size_t)(i + 1) * 88 <= size; ++i)
        if (scene_packet_u32(bytes + 100 + (size_t)i * 88) == surface) return true;
    return false;
}
static struct np_display_scene *packet_scene(struct np_presentation_time *state,
    const void *packet, size_t size)
{
    struct np_presentation_identity identity;
    if (!state || !np_presentation_time_packet_identity(packet, size, &identity) ||
        identity.session != state->session) return NULL;
    struct np_display_scene *scene = find_scene(state, identity.epoch, identity.owner, identity.presentation);
    return scene && scene->sent == identity.sent ? scene : NULL;
}
void np_presentation_time_stamp_scene(struct np_server *server, uint32_t owner,
    uint32_t id, void *packet, size_t size)
{
    struct np_presentation_time *state = server->presentation_time;
    struct np_presentation_identity identity;
    if (!state || !np_presentation_time_packet_identity(packet, size, &identity) ||
        identity.session != state->session || identity.owner != owner || identity.presentation != id ||
        identity.epoch > state->epoch) return;
    uint64_t sent = now_ns();
    struct np_display_scene *scene = packet_scene(state, packet, size);
    if (scene) {
        scene->sent = sent;
        if (identity.epoch == state->epoch && state->clock.valid) scene->clock = state->clock;
    }
    uint32_t packet_owner, packet_id;
    unsigned char *context = packet_context(packet, size, &packet_owner, &packet_id);
    put_u64(context + 16, sent);
}
bool np_presentation_time_prepare_scene(struct np_server *server, uint32_t owner,
    uint32_t id, void *packet, size_t size)
{
    struct np_presentation_time *state = server->presentation_time;
    uint32_t packet_owner, packet_id;
    unsigned char *context = packet_context(packet, size, &packet_owner, &packet_id);
    if (!state || !context || !owner || !id || packet_owner != owner || packet_id != id ||
        !np_presentation_time_emission_allowed(server)) return false;
    /* Reconnect can schedule replay before the old outcome returns. Do not
     * display the same current commit without its still-outstanding query. */
    struct np_display_scene *in_flight;
    wl_list_for_each(in_flight, &state->scenes, link) {
        struct np_display_feedback *feedback;
        wl_list_for_each(feedback, &in_flight->feedbacks, link)
            if (!feedback->retired && packet_samples_surface(packet, size, feedback->surface_id))
                return false;
    }
    /* Only scenes promising public feedback need a trustworthy prior anchor.
     * No-query applications remain free to render during calibration failure. */
    struct np_surface *surface;
    bool queried = false;
    wl_list_for_each(surface, &server->surfaces, link) {
        struct np_display_feedback *feedback;
        if (!has_list(surface)) continue;
        wl_list_for_each(feedback, &surface->presentation_feedbacks, link) {
            if (!feedback->scene || !packet_samples_surface(packet, size, feedback->surface_id)) continue;
            feedback->owner = owner;
            feedback->scene = id;
            queried = true;
            if (state->faulted) {
                feedback_fail(feedback, "NativePipe presentation clock is unavailable for this scene");
                return false;
            }
            if (!state->clock.valid) return false;
        }
    }
    if (!memcmp(packet, "NPW2", 4)) {
        unsigned char *flags = (unsigned char *)packet + 40;
        uint32_t value = scene_packet_u32(flags);
        value = queried ? value | (1u << 6) : value & ~(1u << 6);
        for (unsigned i = 0; i < 4; ++i) flags[i] = value >> (i * 8);
    }
    uint64_t sent = now_ns();
    put_u64(context, state->session);
    put_u64(context + 8, state->epoch);
    put_u64(context + 16, sent);
    if (!queried) return true; /* Host results still receive idempotent ACKs. */
    struct np_display_scene *scene = find_scene(state, state->epoch, owner, id);
    if (!scene) {
        if (wl_list_length(&state->scenes) >= (int)MAX_SCENES) return false;
        scene = calloc(1, sizeof(*scene));
        if (!scene) return false;
        scene->state = state; scene->owner = owner; scene->scene = id; scene->epoch = state->epoch;
        wl_list_init(&scene->feedbacks);
        wl_list_insert(state->scenes.prev, &scene->link);
    }
    /* Last operation before actual send; queue/retry/build time is excluded. */
    scene->sent = scene->created = sent;
    scene->clock = state->clock;
    return true;
}
void np_presentation_time_submitted(struct np_server *server, const void *packet, size_t size)
{
    struct np_presentation_time *state = server->presentation_time;
    struct np_display_scene *scene = packet_scene(state, packet, size);
    if (!scene || scene->submitted) return;
    scene->submitted = true;
    struct np_surface *surface;
    wl_list_for_each(surface, &server->surfaces, link) {
        if (!has_list(surface)) continue;
        struct np_display_feedback *feedback, *tmp;
        wl_list_for_each_safe(feedback, tmp, &surface->presentation_feedbacks, link) {
            if (feedback->scene != scene->scene || feedback->owner != scene->owner ||
                !packet_samples_surface(packet, size, feedback->surface_id)) continue;
            wl_list_remove(&feedback->link);
            wl_list_insert(scene->feedbacks.prev, &feedback->link);
            feedback->surface = NULL;
        }
    }
}
void np_presentation_time_reject_scene(struct np_server *server, const void *packet, size_t size)
{
    struct np_presentation_time *state = server->presentation_time;
    struct np_display_scene *scene = packet_scene(state, packet, size);
    /* A preparation is not a submitted frame. Its queries stay on the surface
     * and can join the same dirty scene on the next admission attempt. */
    if (!scene || scene->submitted) return;
    scene_destroy(scene);
    finish_drain(state);
}
void np_presentation_time_discard_commit(struct np_surface *surface, uint32_t commit)
{
    if (!has_list(surface) || !commit) return;
    retire_submitted_feedback(surface, commit, true);
    struct np_display_feedback *feedback, *tmp;
    wl_list_for_each_safe(feedback, tmp, &surface->presentation_feedbacks, link)
        if (feedback->commit == commit) discard(feedback);
}
void np_presentation_time_discard_scene(struct np_server *server, uint32_t id)
{
    /* Retire an unsent attempt. Its current surface commits may be shown by
     * replay; only commit replacement/detach/destruction discards queries. */
    struct np_presentation_time *state = server->presentation_time;
    if (!state) return;
    struct np_display_scene *scene, *tmp;
    wl_list_for_each_safe(scene, tmp, &state->scenes, link)
        if (scene->scene == id && !scene->submitted) scene_destroy(scene);
}
void np_presentation_time_discard_surface(struct np_surface *surface)
{
    if (!has_list(surface)) return;
    retire_submitted_feedback(surface, 0, false);
    struct np_display_feedback *feedback, *tmp;
    wl_list_for_each_safe(feedback, tmp, &surface->presentation_feedbacks, link) discard(feedback);
    /* Child wl_surfaces have their own commits and may survive their parent. */
}
static void clock_fault(struct np_presentation_time *state)
{
    fprintf(stderr, "[wayland] presentation clock changed outside a sealed epoch\n");
    struct np_display_scene *scene, *tmp;
    wl_list_for_each_safe(scene, tmp, &state->scenes, link) {
        /* The last independently validated sample bounds known history. It
         * does not locate the discontinuity between it and the new sample. */
        uint64_t ceiling = state->clock.host > scene->clock.host ? state->clock.host : scene->clock.host;
        if (state->clock.epoch_host > ceiling) ceiling = state->clock.epoch_host;
        if (scene->clock.epoch_host > ceiling) ceiling = scene->clock.epoch_host;
        if (state->disconnected_clock.valid && state->disconnected_clock.host > ceiling)
            ceiling = state->disconnected_clock.host;
        if (state->disconnected_clock.valid && state->disconnected_clock.epoch_host > ceiling)
            ceiling = state->disconnected_clock.epoch_host;
        if (!scene->mapping_ceiling || ceiling < scene->mapping_ceiling) scene->mapping_ceiling = ceiling;
        if (scene->terminal) flush_result(scene);
    }
    state->clock.valid = false;
    state->faulted = true;
    state->calibration_started = now_ns();
}
void np_presentation_time_clock_sample(struct np_server *server, uint32_t token,
    uint64_t session, uint64_t epoch, uint64_t host)
{
    struct np_presentation_time *state = server->presentation_time;
    if (!state) return;
    if (session != state->session || epoch != state->epoch || !token || token != state->token) {
        np_debug_log("[wayland] presentation clock sample rejected: stale identity token=%u expected=%u session=%llu epoch=%llu\n",
            token, state->token, (unsigned long long)session, (unsigned long long)epoch);
        return;
    }
    uint64_t received = now_ns();
    state->token = 0;
    if (state->history_unverified && np_presentation_clock_discontinuous(
        &state->disconnected_clock, state->sent, received, host)) clock_fault(state);
    if (np_presentation_clock_discontinuous(&state->clock, state->sent, received, host)) {
        np_debug_log("[wayland] presentation clock sample rejected: offset discontinuity token=%u rtt_ns=%llu\n",
            token, (unsigned long long)(received - state->sent));
        clock_fault(state); return;
    }
    if (!np_presentation_clock_sample_at(&state->clock, state->sent, received, host)) {
        const char *reason = !host ? "zero host time" : received < state->sent ? "reversed guest time" :
            received - state->sent > np_presentation_clock_maximum_rtt(&state->clock) ? "response window exceeded" :
            received < state->clock.sampled_at ? "older guest sample" : "older host sample";
        np_debug_log("[wayland] presentation clock sample rejected: %s token=%u rtt_ns=%llu limit_ns=%llu\n",
            reason, token, (unsigned long long)(received >= state->sent ? received - state->sent : 0),
            (unsigned long long)np_presentation_clock_maximum_rtt(&state->clock));
        return;
    }
    np_debug_log("[wayland] presentation clock calibrated token=%u measured_rtt_ns=%llu anchor_rtt_ns=%llu uncertainty_ns=%llu\n",
        token, (unsigned long long)(received - state->sent), (unsigned long long)state->clock.rtt,
        (unsigned long long)(state->clock.rtt / 2));
    state->clock.epoch = epoch;
    /* A slower validated sample may preserve the best mapping. Its instant
     * still advances the trustworthy historical boundary for a later fault. */
    state->clock.epoch_host = host;
    state->calibration_started = 0;
    state->faulted = false;
    state->history_unverified = false;
    state->disconnected_clock.valid = false;
    if (state->capable) np_presentation_time_advertise(server);
    if (state->reconnecting) {
        state->reconnecting = false;
        if (!state->resume_token) state->paused = false;
    }
    if (state->resume_token) {
        uint32_t fields[] = { session, session >> 32, epoch, epoch >> 32, state->resume_token };
        np_window_event_send(server, NP_GUEST_PRESENTATION_RESUMED, fields, 5);
        state->completed_resume = state->resume_token;
        state->resume_token = 0;
        state->paused = false;
    }
    struct np_display_scene *scene, *tmp;
    wl_list_for_each_safe(scene, tmp, &state->scenes, link)
        if (scene->terminal) flush_result(scene);
}
void np_presentation_time_scene_clock_sample(struct np_server *server, uint64_t session,
    uint64_t epoch, uint32_t owner, uint32_t id, uint64_t sent, uint64_t host)
{
    struct np_presentation_time *state = server->presentation_time;
    if (!state || session != state->session || epoch != state->epoch) return;
    struct np_display_scene *scene = find_scene(state, epoch, owner, id);
    if (!scene || scene->sent != sent || !host || (scene->terminal && scene->host && host > scene->host)) return;
    uint64_t received = now_ns();
    if (np_presentation_clock_discontinuous(&scene->clock, sent, received, host)) {
        clock_fault(state); return;
    }
    if (np_presentation_clock_sample_at(&scene->clock, sent, received, host)) {
        scene->clock.epoch_host = host;
        flush_result(scene);
    }
}
void np_presentation_time_result(struct np_server *server, uint64_t session, uint64_t epoch,
    uint32_t owner, uint32_t id, uint64_t host, uint32_t refresh, uint32_t output)
{
    (void)refresh;
    struct np_presentation_time *state = server->presentation_time;
    if (!state || session != state->session || !epoch || epoch > state->epoch || !owner || !id) return;
    struct np_display_scene *scene = find_scene(state, epoch, owner, id);
    if (!scene) { result_ack(state, epoch, owner, id); return; }
    if (!scene->submitted) return;
    if (!scene->terminal) { scene->terminal = true; scene->host = host; scene->output = output; }
    flush_result(scene);
}
void np_presentation_time_pause(struct np_server *server, uint64_t session, uint32_t token)
{
    struct np_presentation_time *state = server->presentation_time;
    if (!state || session != state->session || !token) return;
    if (token == state->pause_token && state->paused && !state->resume_token) {
        if (state->pause_reached) send_fence(state, NP_GUEST_PRESENTATION_PAUSE_REACHED, token);
        else finish_pause(state);
        return;
    }
    if (state->last_barrier && (int32_t)(token - state->last_barrier) <= 0) return;
    state->last_barrier = token;
    state->paused = true;
    state->pause_token = token;
    state->pause_reached = false;
    /* np_window_event_send uses the display lane, after all prior scenes. */
    finish_pause(state);
}
void np_presentation_time_drain(struct np_server *server, uint64_t session, uint32_t token)
{
    struct np_presentation_time *state = server->presentation_time;
    if (!state || session != state->session || !token || !state->paused ||
        !state->pause_reached || token != state->pause_token) return;
    state->drain_token = token;
    finish_drain(state);
}
void np_presentation_time_resume(struct np_server *server, uint64_t session, uint32_t token)
{
    struct np_presentation_time *state = server->presentation_time;
    if (!state || session != state->session || !token) return;
    if (token == state->completed_resume && token == state->last_barrier) {
        uint32_t fields[] = { session, session >> 32, state->epoch, state->epoch >> 32, token };
        np_window_event_send(server, NP_GUEST_PRESENTATION_RESUMED, fields, 5); return;
    }
    if (token == state->resume_token) return;
    if (state->last_barrier && (int32_t)(token - state->last_barrier) <= 0) return;
    state->last_barrier = token;
    ++state->epoch;
    if (!state->epoch) ++state->epoch;
    if (state->clock.valid) state->disconnected_clock = state->clock;
    if (!wl_list_empty(&state->scenes)) state->history_unverified = true;
    state->clock = (struct np_presentation_clock){ .epoch = state->epoch,
        .maximum_rtt_ns = state->clock.maximum_rtt_ns };
    state->faulted = false;
    state->paused = true;
    state->token = 0; state->drain_token = 0;
    state->resume_token = token;
    state->calibration_started = now_ns();
    np_presentation_time_clock_request(server);
}
void np_presentation_time_reset(struct np_server *server)
{
    struct np_presentation_time *state = server->presentation_time;
    if (!state) return;
    /* Submitted records and historical anchors belong to the guest process,
     * not a vsock connection. Host replay may finish them later. */
    state->token = 0;
    if (state->clock.valid) state->disconnected_clock = state->clock;
    state->clock.valid = false;
    state->faulted = false;
    state->last_barrier = state->pause_token = state->drain_token = 0;
    state->resume_token = state->completed_resume = 0;
    state->pause_reached = false;
    state->reconnecting = true;
    state->history_unverified = true;
    state->paused = false;
    state->calibration_started = now_ns();
    struct np_display_scene *scene, *tmp;
    wl_list_for_each_safe(scene, tmp, &state->scenes, link)
        if (!scene->submitted) scene_destroy(scene);
    /* Unsubmitted queries remain attached to their commit. Replay rebinds
     * current ones; cached/pending commits retain their ordinary lifetime. */
}
void np_presentation_time_destroy(struct np_server *server)
{
    struct np_presentation_time *state = server->presentation_time;
    if (!state) return;
    struct np_display_scene *scene, *tmp;
    wl_list_for_each_safe(scene, tmp, &state->scenes, link) scene_destroy(scene);
    struct np_surface *surface;
    wl_list_for_each(surface, &server->surfaces, link) np_presentation_time_discard_surface(surface);
    wl_event_source_remove(state->timer);
    if (state->global) wl_global_destroy(state->global);
    server->presentation_time = NULL;
    free(state);
}

bool np_presentation_time_handle_command(struct np_server *server, struct np_window_reader *reader)
{
    if (reader->opcode == NP_HOST_PRESENTATION_CLOCK_SAMPLE) {
        uint32_t token = np_window_read_u32(reader);
        uint64_t session = np_window_read_u64(reader), epoch = np_window_read_u64(reader);
        uint64_t host = np_window_read_u64(reader);
        if (!np_window_reader_finished(reader)) return false;
        np_presentation_time_clock_sample(server, token, session, epoch, host);
        return true;
    }
    if (reader->opcode == NP_HOST_PRESENTATION_FEEDBACK || reader->opcode == NP_HOST_SCENE_CLOCK_SAMPLE) {
        uint64_t session = np_window_read_u64(reader), epoch = np_window_read_u64(reader);
        uint32_t owner = np_window_read_u32(reader), scene = np_window_read_u32(reader);
        uint64_t time = np_window_read_u64(reader);
        if (reader->opcode == NP_HOST_SCENE_CLOCK_SAMPLE) {
            uint64_t received = np_window_read_u64(reader);
            if (!np_window_reader_finished(reader)) return false;
            np_presentation_time_scene_clock_sample(server, session, epoch, owner, scene, time, received);
        } else {
            uint32_t refresh = np_window_read_u32(reader), output = np_window_read_u32(reader);
            if (!np_window_reader_finished(reader)) return false;
            np_presentation_time_result(server, session, epoch, owner, scene, time, refresh, output);
        }
        return true;
    }
    if (reader->opcode == NP_HOST_PRESENTATION_PAUSE || reader->opcode == NP_HOST_PRESENTATION_DRAIN ||
        reader->opcode == NP_HOST_PRESENTATION_RESUME) {
        uint64_t session = np_window_read_u64(reader);
        uint32_t token = np_window_read_u32(reader);
        if (!np_window_reader_finished(reader)) return false;
        if (reader->opcode == NP_HOST_PRESENTATION_PAUSE) np_presentation_time_pause(server, session, token);
        else if (reader->opcode == NP_HOST_PRESENTATION_DRAIN) np_presentation_time_drain(server, session, token);
        else np_presentation_time_resume(server, session, token);
        return true;
    }
    return false;
}
