#include "backend.h"

#include "compositor_internal.h"
#include "backend_internal.h"
#include "scene.h"
#include "surface_internal.h"
#include "window_events.h"

#include <string.h>
#include <stdlib.h>

static void completed(struct np_server *server, uint32_t owner, uint32_t presentation,
                      bool scene, bool displayed)
{
    struct np_remote_backend *b = np_remote_backend(server);
    struct np_surface *s = np_surface_by_id(server, owner);
    struct np_remote_surface *state = s ? s->backend_surface_state : NULL;
    if (!state) return;
    for (unsigned i = 0; i < state->flight_count; i++) {
        if (state->flight[i].id != presentation || state->flight[i].scene != scene) continue;
        state->flight[i] = state->flight[--state->flight_count];
        if (scene) {
            if (displayed) b->displayed_scenes++; else b->discarded_scenes++;
        }
        return;
    }
}
void np_backend_host_presented(struct np_server *server, uint32_t owner, uint32_t presentation)
{
    /* Cursor/drag-image copies have no window drawable. Window scenes wait
     * for NPRP, not the shared frame/FIFO latch notification. */
    completed(server, owner, presentation, false, true);
}
void np_remote_scene_completed(struct np_server *server, uint32_t owner, uint32_t presentation, bool displayed, uint32_t interval_ns)
{
    struct np_surface *surface = np_surface_by_id(server, owner);
    struct np_remote_surface *state = surface ? surface->backend_surface_state : NULL;
    if (state && interval_ns) state->display_interval_ns = interval_ns;
    completed(server, owner, presentation, true, displayed);
}

bool np_backend_scene_ready(const struct np_surface *surface)
{
    struct np_remote_backend *b = np_remote_backend(surface->server);
    if (!np_remote_scene_available(surface) || !np_media_can_encode(&b->media)) return false;
    /* Rotate only among eligible dirty owners: a slow, hidden or paced window
     * must not prevent another window from taking the available slot. */
    if (b->previous_owner == surface->id) {
        struct np_surface *other;
        wl_list_for_each(other, &surface->server->surfaces, link) {
            if (other != surface && ((other->scene_dirty && np_scene_root(other) == other) ||
                (other->pending_frame && !np_scene_root(other))) && np_remote_scene_available(other)) return false;
        }
    }
    return true;
}

bool np_backend_surface_has_current(const struct np_surface *surface)
{
	return surface && surface->has_published && surface->last_resource_id != 0;
}

bool np_backend_describe_scene_source(
	const struct np_surface *surface, struct np_backend_scene_source *source)
{
	if (!np_backend_surface_has_current(surface) || !source) return false;
	memset(source, 0, sizeof(*source));
	source->resource_id = surface->last_resource_id;
	source->stride = surface->last_stride;
	/* Decoder output is BGRA after optional alpha reconstruction. */
	source->format = 1u;
	source->flags = surface->last_format &&
	                strcmp(surface->last_format, "bgrx8888") == 0 ? 1u : 0u;
	return true;
}

enum np_backend_hold_result np_backend_hold_scene(
	struct np_surface *owner, uint32_t presentation_id,
	struct np_surface *const *surfaces, size_t surface_count)
{
	if (!owner || !presentation_id || !surfaces || !surface_count)
		return NP_BACKEND_HOLD_INVALID;
	for (size_t i = 0; i < surface_count; i++) {
		if (!np_backend_surface_has_current(surfaces[i]))
			return NP_BACKEND_HOLD_INVALID;
	}
	return NP_BACKEND_HOLD_READY;
}

void np_backend_release_scene(
	struct np_surface *owner, uint32_t presentation_id)
{
	(void)owner;
	(void)presentation_id;
}

void np_backend_discard_scenes(struct np_surface *owner)
{
	(void)owner;
}

void np_backend_configure_frame(
	const struct np_surface *surface, struct np_window_frame *frame)
{
	if (!surface || !frame) return;
	const struct np_remote_surface *state = surface->backend_surface_state;
	frame->source = NP_WINDOW_FRAME_ENCODED;
	frame->bitstream_epoch = state ? state->last_epoch : 0;
	frame->codec = "h264";
}
