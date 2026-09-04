#include "backend.h"

#include "compositor_internal.h"
#include "dmabuf.h"
#include "shm_texture.h"
#include "shm_texture_internal.h"
#include "window_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NP_VMPIPE_SCENE_MAX_REFERENCES = 128 };

enum np_scene_reference_kind { NP_SCENE_GPU, NP_SCENE_SHM };

struct np_scene_reference {
	enum np_scene_reference_kind kind;
	union {
		struct np_gpu_buffer *gpu;
		struct np_shm_texture *shm;
	};
};

struct np_scene_presentation {
	struct wl_list link;
	uint32_t id;
	uint32_t reference_count;
	struct np_scene_reference references[NP_VMPIPE_SCENE_MAX_REFERENCES];
};

bool np_backend_surface_has_current(const struct np_surface *surface)
{
	return surface && (surface->current_gpu || surface->current_shm);
}

bool np_backend_describe_scene_source(
	const struct np_surface *surface, struct np_backend_scene_source *source)
{
	if (!surface || !source) return false;
	memset(source, 0, sizeof(*source));
	if (surface->current_gpu) {
		source->resource_id = surface->current_gpu->resource_id;
		source->stride = (uint32_t)surface->current_gpu->stride;
	} else if (surface->current_shm) {
		source->resource_id = surface->current_shm->image.resource_id;
		source->stride = surface->current_shm->stride;
	} else {
		return false;
	}
	source->format = surface->last_format &&
	                 strcmp(surface->last_format, "bgrx8888") == 0 ? 2u :
	                 surface->last_format &&
	                 strcmp(surface->last_format, "rgba8888") == 0 ? 3u : 1u;
	source->flags = source->format == 2u ? 1u : 0u;
	return source->resource_id != 0;
}

static void release_references(struct np_scene_presentation *presentation)
{
	for (uint32_t i = 0; i < presentation->reference_count; i++) {
		struct np_scene_reference *reference = &presentation->references[i];
		if (reference->kind == NP_SCENE_GPU)
			np_gpu_buffer_end_host_read(reference->gpu);
		else
			np_shm_texture_end_host_read(reference->shm);
	}
	presentation->reference_count = 0;
}

static bool same_reference(const struct np_scene_reference *a,
	                       const struct np_scene_reference *b)
{
	if (a->kind != b->kind) return false;
	return a->kind == NP_SCENE_GPU ? a->gpu == b->gpu : a->shm == b->shm;
}

static bool retain_reference(struct np_scene_presentation *presentation,
	                         struct np_scene_reference reference)
{
	for (uint32_t i = 0; i < presentation->reference_count; i++) {
		if (same_reference(&presentation->references[i], &reference)) return true;
	}
	if (presentation->reference_count >= NP_VMPIPE_SCENE_MAX_REFERENCES)
		return false;
	if (reference.kind == NP_SCENE_GPU) {
		if (!np_gpu_buffer_acquire_host_read(reference.gpu)) return false;
	} else {
		np_shm_texture_begin_host_read(reference.shm);
	}
	presentation->references[presentation->reference_count++] = reference;
	return true;
}

enum np_backend_hold_result np_backend_hold_scene(
	struct np_surface *owner, uint32_t presentation_id,
	struct np_surface *const *surfaces, size_t surface_count)
{
	if (!owner || !presentation_id || !surfaces || !surface_count ||
	    surface_count > NP_VMPIPE_SCENE_MAX_REFERENCES)
		return NP_BACKEND_HOLD_INVALID;
	struct np_scene_presentation *presentation = calloc(1, sizeof(*presentation));
	if (!presentation) return NP_BACKEND_HOLD_NO_MEMORY;
	wl_list_init(&presentation->link);
	presentation->id = presentation_id;
	for (size_t i = 0; i < surface_count; i++) {
		struct np_surface *surface = surfaces[i];
		struct np_scene_reference reference;
		if (surface && surface->current_gpu) {
			reference.kind = NP_SCENE_GPU;
			reference.gpu = surface->current_gpu;
		} else if (surface && surface->current_shm) {
			reference.kind = NP_SCENE_SHM;
			reference.shm = surface->current_shm;
		} else {
			release_references(presentation);
			free(presentation);
			return NP_BACKEND_HOLD_INVALID;
		}
		if (!retain_reference(presentation, reference)) {
			release_references(presentation);
			free(presentation);
			return NP_BACKEND_HOLD_INVALID;
		}
	}
	wl_list_insert(&owner->scene_presentations, &presentation->link);
	return NP_BACKEND_HOLD_READY;
}

void np_backend_release_scene(
	struct np_surface *owner, uint32_t presentation_id)
{
	if (!owner || !presentation_id) return;
	struct np_scene_presentation *presentation, *tmp;
	wl_list_for_each_safe(presentation, tmp, &owner->scene_presentations, link) {
		if (presentation->id != presentation_id) continue;
		if (np_trace_enabled())
			fprintf(stderr,
			        "[scene] release surface=%u present=%u references=%u\n",
			        owner->id, presentation_id, presentation->reference_count);
		wl_list_remove(&presentation->link);
		release_references(presentation);
		free(presentation);
		return;
	}
}

void np_backend_discard_scenes(struct np_surface *owner)
{
	if (!owner) return;
	struct np_scene_presentation *presentation, *tmp;
	wl_list_for_each_safe(presentation, tmp, &owner->scene_presentations, link) {
		wl_list_remove(&presentation->link);
		release_references(presentation);
		free(presentation);
	}
}

void np_backend_configure_frame(
	const struct np_surface *surface, struct np_window_frame *frame)
{
	if (!surface || !frame) return;
	frame->source = surface->last_source &&
	                strcmp(surface->last_source, "gpu") == 0
		? NP_WINDOW_FRAME_GPU : NP_WINDOW_FRAME_CPU;
}
