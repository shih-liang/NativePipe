/* Remote backend publication: copy committed pixels into immutable encoded
 * resources.  Wayland scene and callback scheduling remain in the frontend. */

#define _GNU_SOURCE

#include "backend.h"
#include "backend_internal.h"
#include "compositor_internal.h"
#include "dmabuf.h"
#include "media.h"
#include "scene.h"
#include "scale.h"
#include "surface_internal.h"
#include "syncobj.h"
#include "window_events.h"
#include "windowwire.h"
#include "xdg_shell.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-protocol.h>

struct np_remote_scene_input {
    uint32_t surface_id, resource_id;
    int width, height, stride;
    uint64_t pts_ns;
    uint8_t flags;
    unsigned char *pixels;
};
struct np_remote_scene_job {
    uint32_t owner, presentation;
    unsigned char *packet;
    size_t size, count, next;
    struct np_remote_scene_input *inputs;
    atomic_bool encoding, failed;
    atomic_uint completed;
    bool cancelled;
};

static uint64_t monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static struct np_remote_surface *remote_surface(
	struct np_surface *surface, bool create)
{
	if (!surface) return NULL;
	struct np_remote_surface *state = surface->backend_surface_state;
	if (!state && create) {
		state = calloc(1, sizeof(*state));
		surface->backend_surface_state = state;
	}
	return state;
}

static void encoder_emit(
	void *user, const uint8_t *data, size_t size, uint64_t pts_ns,
	uint32_t resource_id, uint8_t flags, const uint8_t *alpha,
	uint32_t alpha_size, uint16_t bitstream_epoch,
	uint16_t width, uint16_t height, enum np_encoder_codec codec)
{
	struct np_surface *surface = user;
	if (!surface || !surface->server) return;
	struct np_remote_backend *backend = np_remote_backend(surface->server);
	if (!backend) return;
	bool ok = true;
	if (alpha && alpha_size)
		ok = np_media_send(
			&backend->media, NP_MEDIA_CODEC_ALPHA_RLE, 0,
			surface->id, resource_id, width, height, pts_ns,
			bitstream_epoch, alpha, alpha_size);
	ok = np_media_send(
		&backend->media, (uint8_t)codec, flags,
		surface->id, resource_id, width, height, pts_ns,
		bitstream_epoch, data, (uint32_t)size) && ok;
	struct np_remote_surface *state = remote_surface(surface, false);
	if (!ok && state && state->job) atomic_store(&state->job->failed, true);
}

static uint32_t next_media_resource_id(struct np_server *server)
{
	struct np_remote_backend *backend = np_remote_backend(server);
	if (!backend) return 0;
	uint32_t id = ++backend->next_media_resource_id;
	if (!id) id = ++backend->next_media_resource_id;
	return id;
}

static bool source_has_transparency(
	const unsigned char *source, int32_t width, int32_t height, int32_t stride)
{
	for (int32_t row = 0; row < height; row++) {
		const unsigned char *pixel = source + (size_t)row * (size_t)stride + 3;
		for (int32_t column = 0; column < width; column++, pixel += 4) {
			if (*pixel != 255) return true;
		}
	}
	return false;
}

static bool encode_remote_pixels(
	struct np_surface *surface, const unsigned char *source,
	int32_t width, int32_t height, int32_t stride, uint32_t format)
{
	if (!source || width < 1 || height < 1 || width >= UINT16_MAX ||
	    height >= UINT16_MAX || width > INT32_MAX / 4 || stride < width * 4)
		return false;
	if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888)
		return false;
	int even_width = (width + 1) & ~1, even_height = (height + 1) & ~1;
	if (even_width > NP_ENCODER_MAX_DIMENSION || even_height > NP_ENCODER_MAX_DIMENSION ||
	    (uint64_t)even_width * even_height > NP_ENCODER_MAX_PIXELS) return false;
	struct np_remote_surface *state = remote_surface(surface, true);
	if (!state) return false;
	uint32_t resource_id = next_media_resource_id(surface->server);
	uint8_t flags = format == WL_SHM_FORMAT_ARGB8888 &&
	                source_has_transparency(source, width, height, stride)
		? NP_MEDIA_FLAG_HAS_ALPHA : 0;
	size_t row_size = (size_t)even_width * 4;
	if ((size_t)even_height > SIZE_MAX / row_size) return false;
	unsigned char *copy = malloc(row_size * (size_t)even_height);
	if (!copy) return false;
	for (int32_t y = 0; y < height; y++) {
		unsigned char *row = copy + (size_t)y * row_size;
		memcpy(row, source + (size_t)y * stride, (size_t)width * 4);
		if (even_width != width) memcpy(row + (size_t)width * 4, row + (size_t)(width - 1) * 4, 4);
	}
	if (even_height != height)
		memcpy(copy + (size_t)height * row_size, copy + (size_t)(height - 1) * row_size, row_size);
	struct np_remote_backend *backend = np_remote_backend(surface->server);
	backend->captured_frames++;
	if (state->pixels) backend->coalesced_frames++;
	free(state->pixels);
	state->pixels = copy;
	state->flags = flags;
	state->pts_ns = monotonic_ns();
	surface->last_width = width;
	surface->last_height = height;
	surface->last_stride = (uint32_t)(width * 4);
	surface->last_format = format == WL_SHM_FORMAT_ARGB8888
		? "bgra8888" : "bgrx8888";
	surface->last_source = "encoded";
	surface->last_resource_id = resource_id;
	surface->has_published = true;
	return true;
}

static bool publish_frame_remote(
	struct np_surface *surface, struct wl_shm_buffer *shm)
{
	wl_shm_buffer_begin_access(shm);
	bool encoded = encode_remote_pixels(
		surface, wl_shm_buffer_get_data(shm), wl_shm_buffer_get_width(shm),
		wl_shm_buffer_get_height(shm), wl_shm_buffer_get_stride(shm),
		wl_shm_buffer_get_format(shm));
	wl_shm_buffer_end_access(shm);
	return encoded;
}

bool np_backend_refresh_current_shm(
	struct np_surface *surface, uint32_t presentation_id,
	const struct np_box *damage)
{
	(void)damage;
	struct wl_shm_buffer *shm = wl_shm_buffer_get(surface->current_buffer);
	if (!shm || !publish_frame_remote(surface, shm)) return false;
	if (np_scene_root(surface))
		np_presentation_queue_scene(surface, presentation_id);
	else if (!np_presentation_queue_last(surface, presentation_id))
		np_presentation_request_refresh(surface, presentation_id);
	return true;
}

void np_backend_surface_destroy(struct np_surface *surface)
{
	np_remote_cancel_scenes(surface->server, surface->id);
	struct np_remote_surface *state = remote_surface(surface, false);
	if (!state) return;
	if (state->encoder) np_encoder_destroy(state->encoder);
	free(state->pixels);
	free(state);
	surface->backend_surface_state = NULL;
}

static void encoder_finished(void *user, bool ok)
{
    struct np_surface *surface = user;
    struct np_remote_backend *b = np_remote_backend(surface->server);
    struct np_remote_scene_job *job = remote_surface(surface, false)->job;
    if (!ok) atomic_store(&job->failed, true);
    else atomic_fetch_add(&job->completed, 1);
    /* Last access to job. The event loop may free it after this release; the
     * surface/backend survive until np_encoder_destroy has joined us. */
    atomic_store_explicit(&job->encoding, false, memory_order_release);
    np_media_wake(&b->media);
}

static uint32_t get32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int refresh_ready(void *data)
{
    struct np_server *server = data;
    np_remote_backend(server)->refresh_deadline = 0;
    np_remote_flush_encoded(server);
    np_presentation_flush(server);
    wl_display_flush_clients(server->display);
    return 0;
}

bool np_remote_scene_available(const struct np_surface *surface)
{
    struct np_remote_backend *b = np_remote_backend(surface->server);
    struct np_remote_surface *state = surface->backend_surface_state;
    if ((state && (state->job || state->flight_count >= 8)) ||
        (b->jobs[0] && b->jobs[1])) return false;
    for (unsigned i = 0; i < 2; i++)
        if (b->jobs[i] && b->jobs[i]->owner == surface->id) return false;
    uint64_t now = monotonic_ns();
    if (!state || now >= state->next_refresh_ns) return true;
    /* Pacing happens before copying into an encode job. A refresh wait holds
     * neither the global encoder budget nor an immutable stale candidate. */
    if (!b->refresh_timer)
        b->refresh_timer = wl_event_loop_add_timer(
            wl_display_get_event_loop(surface->server->display), refresh_ready, surface->server);
    if (!b->refresh_timer) { surface->server->terminate = true; b->exit_status = 1; return false; }
    if (!b->refresh_deadline || state->next_refresh_ns < b->refresh_deadline) {
        b->refresh_deadline = state->next_refresh_ns;
        wl_event_source_timer_update(b->refresh_timer,
            (int)((state->next_refresh_ns - now + 999999) / 1000000));
    }
    return false;
}

bool np_remote_submit_scene(struct np_server *server, const void *data, size_t size)
{
    struct np_remote_backend *b = np_remote_backend(server);
    const unsigned char *p = data;
    if (size < 40 || !np_media_can_encode(&b->media)) return false;
    bool scene = !memcmp(p, "NPSN", 4);
    if (scene && size < 76) return false;
    struct np_surface *owner = np_surface_by_id(server, get32(p + (scene ? 12 : 8)));
    if (!owner || !remote_surface(owner, true) || !np_remote_scene_available(owner)) return false;
    size_t count = scene ? get32(p + 48) : 1;
    if (!count || count > 128 || (scene && size != 76 + count * 88)) return false;
    struct np_remote_scene_job *job = calloc(1, sizeof(*job));
    if (!job) return false;
    job->inputs = calloc(count, sizeof(*job->inputs));
    job->packet = malloc(size);
    if (!job->inputs || !job->packet) goto reject;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *source = scene ? p + 76 + i * 88 : p + 8;
        struct np_surface *s = np_surface_by_id(server, get32(source));
        struct np_remote_surface *state = remote_surface(s, false);
        if (!state || state->job || s->last_resource_id != get32(source + 4)) goto reject;
        for (size_t k = 0; k < i; k++) if (job->inputs[k].surface_id == s->id) goto reject;
        job->inputs[i] = (struct np_remote_scene_input){
            .surface_id = s->id, .resource_id = s->last_resource_id,
            .width = (s->last_width + 1) & ~1, .height = (s->last_height + 1) & ~1,
            .stride = ((s->last_width + 1) & ~1) * 4, .pts_ns = state->pts_ns,
            .flags = state->flags, .pixels = state->pixels };
    }
    memcpy(job->packet, data, size);
    job->count = count; job->size = size;
    job->owner = owner->id; job->presentation = get32(p + (scene ? 16 : 36));
    atomic_init(&job->encoding, false); atomic_init(&job->failed, false); atomic_init(&job->completed, 0);
    for (size_t i = 0; i < count; i++) {
        struct np_remote_surface *state = remote_surface(np_surface_by_id(server, job->inputs[i].surface_id), false);
        state->job = job; state->pixels = NULL;
    }
    b->jobs[b->jobs[0] ? 1 : 0] = job;
    b->previous_owner = owner->id;
    struct np_remote_surface *state = remote_surface(owner, false);
    state->next_refresh_ns = monotonic_ns() + (state->display_interval_ns ? state->display_interval_ns :
        1000000000000ull / (uint32_t)np_scale_surface_refresh_millihz(owner));
    np_media_wake(&b->media);
    return true;
reject:
    free(job->inputs); free(job->packet); free(job);
    return false;
}

void np_remote_cancel_scenes(struct np_server *server, uint32_t surface_id)
{
    struct np_remote_backend *b = np_remote_backend(server);
    for (unsigned i = 0; i < 2; i++) {
        struct np_remote_scene_job *job = b->jobs[i];
        if (!job) continue;
        if (job->owner == surface_id) job->cancelled = true;
        for (size_t k = 0; k < job->count; k++)
            if (job->inputs[k].surface_id == surface_id) job->cancelled = true;
    }
}

static void free_scene(struct np_server *server, struct np_remote_scene_job *job)
{
    for (size_t i = 0; i < job->count; i++) {
        struct np_remote_surface *state = remote_surface(np_surface_by_id(server, job->inputs[i].surface_id), false);
        if (state && state->job == job) {
            state->job = NULL;
            struct np_surface *s = np_surface_by_id(server, job->inputs[i].surface_id);
            /* Cancellation may remove only a sibling. Keep the latest source
             * candidate if its pixels never reached the encoder. */
            if (job->inputs[i].pixels && !state->pixels && s->last_resource_id == job->inputs[i].resource_id) {
                state->pixels = job->inputs[i].pixels; job->inputs[i].pixels = NULL;
            }
        }
        free(job->inputs[i].pixels);
    }
    free(job->inputs); free(job->packet); free(job);
}

void np_remote_finish_scenes(struct np_server *server)
{
    struct np_remote_backend *b = np_remote_backend(server);
    /* All surfaces/workers have been destroyed and joined first. */
    for (unsigned i = 0; i < 2; i++) if (b->jobs[i]) {
        free_scene(server, b->jobs[i]); b->jobs[i] = NULL;
    }
    if (b->refresh_timer) { wl_event_source_remove(b->refresh_timer); b->refresh_timer = NULL; }
}

void np_remote_flush_encoded(struct np_server *server)
{
    struct np_remote_backend *b = np_remote_backend(server);
    for (unsigned slot = 0; slot < 2; slot++) {
        struct np_remote_scene_job *job = b->jobs[slot];
        if (!job || atomic_load_explicit(&job->encoding, memory_order_acquire)) continue;
        b->encoded_frames += atomic_exchange(&job->completed, 0);
        while (!job->cancelled && !atomic_load(&job->failed) && job->next < job->count) {
            struct np_remote_scene_input *input = &job->inputs[job->next++];
            if (!input->pixels) continue;
            struct np_surface *s = np_surface_by_id(server, input->surface_id);
            struct np_remote_surface *state = remote_surface(s, false);
            if (!state) { job->cancelled = true; break; }
            if (!state->encoder)
                state->encoder = np_encoder_create(input->width, input->height,
                    b->host_h264_hardware, encoder_emit, encoder_finished, s);
            atomic_store(&job->encoding, true);
            if (state->encoder && np_encoder_take_bgra(state->encoder, input->pixels,
                    input->width, input->height, input->stride, input->pts_ns, input->resource_id, input->flags)) {
                input->pixels = NULL;
            } else encoder_finished(s, false);
            break;
        }
        if (atomic_load_explicit(&job->encoding, memory_order_acquire)) continue;
        if (!job->cancelled && !atomic_load(&job->failed) && job->next < job->count) continue;
        struct np_surface *owner = np_surface_by_id(server, job->owner);
        struct np_remote_surface *state = remote_surface(owner, false);
        bool sent = state && !job->cancelled && !atomic_load(&job->failed);
        if (atomic_load(&job->failed) || (sent && !np_media_send_display(&b->media, job->packet, job->size))) {
            server->terminate = true; b->exit_status = 1; sent = false;
        }
        uint32_t owner_id = job->owner, presentation_id = job->presentation;
        if (sent) {
            b->sent_scenes++;
            unsigned i = state->flight_count++;
            state->flight[i].id = presentation_id;
            state->flight[i].scene = !memcmp(job->packet, "NPSN", 4);
        }
        free_scene(server, job); b->jobs[slot] = NULL;
        /* Virtual-output latch permits new client work. NPRP independently
         * returns host-display credit; byte ACKs and pixel use are separate. */
        if (sent) {
            np_xdg_flush_pending_toplevel_configure(owner);
            np_presentation_process_presented(server, owner_id, presentation_id);
        }
    }
}

void np_backend_publish_buffer(
	struct np_surface *surface, struct wl_resource *buffer,
	struct np_gpu_buffer *gpu_buffer,
	enum np_buffer_commit_kind buffer_commit, uint32_t presentation_id,
	struct np_sync_point *release_point, const struct np_box *damage)
{
	struct np_surface *root = np_scene_root(surface);
	(void)damage;
	if (buffer_commit == NP_BUFFER_DETACH) {
		if (surface == root && surface->mapped) {
			uint32_t fields[] = {surface->id};
			np_window_event_send(surface->server, NP_GUEST_SURFACE_UNMAPPED,
			                     fields, 1);
			surface->mapped = false;
		}
		np_presentation_set_current_buffer(surface, NULL, NULL, release_point);
		surface->has_published = false;
		if (root && surface != root)
			np_presentation_queue_scene(surface, presentation_id);
		else
			np_presentation_request_refresh(surface, presentation_id);
		return;
	}

	struct wl_shm_buffer *shm = buffer ? wl_shm_buffer_get(buffer) : NULL;
	struct np_gpu_buffer *gpu = gpu_buffer ? gpu_buffer : np_gpu_buffer_get(buffer);
	bool encoded = false;
	if (shm) {
		encoded = publish_frame_remote(surface, shm);
	} else if (gpu) {
		const unsigned char *pixels = NULL;
		if (np_gpu_buffer_begin_cpu_read(gpu, &pixels)) {
			encoded = encode_remote_pixels(
				surface, pixels, gpu->width, gpu->height,
				gpu->stride, gpu->format);
			np_gpu_buffer_end_cpu_read(gpu);
		}
	}
	if (!encoded) {
		np_sync_point_signal(release_point);
		if (buffer) wl_buffer_send_release(buffer);
		wl_client_post_implementation_error(
			wl_resource_get_client(surface->resource),
			"could not copy the committed buffer into the remote encoder");
		return;
	}

	np_presentation_set_current_buffer(surface, buffer, gpu, release_point);
	if (buffer) wl_buffer_send_release(buffer);
	if (root) {
		if (surface == root) surface->mapped = true;
		np_presentation_queue_scene(surface, presentation_id);
	} else if (!np_presentation_queue_last(surface, presentation_id)) {
		np_presentation_request_refresh(surface, presentation_id);
	}
}
