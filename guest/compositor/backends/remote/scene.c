#include "backend.h"

#include "compositor_internal.h"
#include "surface_internal.h"
#include "window_events.h"

#include <string.h>

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
