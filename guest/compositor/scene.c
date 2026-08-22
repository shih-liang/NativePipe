#include "scene.h"

#include "compositor_internal.h"
#include "dmabuf.h"
#include "gpu_copy.h"
#include "perf.h"
#include "scale.h"
#include "scene_renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#ifdef NP_REMOTE

struct np_surface *np_scene_root(struct np_surface *surface)
{
	while (surface && surface->parent)
		surface = surface->parent;
	return surface && (surface->toplevel || surface->popup) ? surface : NULL;
}

bool np_scene_compose(struct np_surface *root, uint32_t presentation_id,
	                  struct np_scene_frame *frame)
{
	(void)root; (void)presentation_id; (void)frame;
	return false;
}

void np_scene_presented(struct np_surface *root, uint32_t presentation_id)
{
	(void)root; (void)presentation_id;
}

void np_scene_destroy(struct np_surface *surface) { (void)surface; }

#else

static bool scene_trace_enabled(void)
{
	return getenv("NP_TRACE") != NULL ||
	       access("/run/nativepipe-trace", F_OK) == 0;
}

struct np_surface *np_scene_root(struct np_surface *surface)
{
	while (surface && surface->parent)
		surface = surface->parent;
	return surface && (surface->toplevel || surface->popup) ? surface : NULL;
}

static uint32_t grow_extent(uint32_t current, uint32_t required, uint32_t quantum)
{
	uint64_t target = required;
	if (current) {
		uint64_t grown = (uint64_t)current + current / 2u;
		if (grown > target) target = grown;
	}
	target = (target + quantum - 1u) / quantum * quantum;
	return target > UINT32_MAX ? 0 : (uint32_t)target;
}

static bool ensure_output(struct np_surface *root, uint32_t width, uint32_t height)
{
	struct np_scene_output *output = &root->scene;
	if (output->images[0].data && width <= output->alloc_width &&
	    height <= output->alloc_height)
		return true;

	uint32_t alloc_width = grow_extent(output->alloc_width, width, 128);
	uint32_t alloc_height = grow_extent(output->alloc_height, height, 64);
	if (!alloc_width || !alloc_height || alloc_width > UINT32_MAX / 4u)
		return false;

	struct np_scene_output replacement = { 0 };

	size_t requested = (size_t)alloc_width * 4u * alloc_height;
	if (!np_blob_create(root->server->drm_fd, requested,
	                    alloc_width, alloc_height, alloc_width * 4u,
	                    &replacement.images[0])) {
		return false;
	}
	replacement.alloc_width = alloc_width;
	replacement.alloc_height = alloc_height;
	replacement.stride = replacement.images[0].vk.stride;

	if (output->images[0].data) {
		struct np_retired_scene_output *retired = calloc(1, sizeof(*retired));
		if (!retired) {
			np_blob_destroy(root->server->drm_fd, &replacement.images[0]);
			return false;
		}
		retired->output = *output;
		retired->next = root->retired_scenes;
		root->retired_scenes = retired;
	}
	*output = replacement;
	return true;
}

static int acquire_slot(struct np_scene_output *output)
{
	return output->presentation_id[0] ? -1 : 0;
}

#define NP_SCENE_MAX_LAYERS 128u

struct scene_item {
	struct np_surface *surface;
	struct wl_shm_buffer *shm;
	struct np_gpu_buffer *gpu_buffer;
	struct np_scene_layer layer;
	bool gpu;
};

static bool collect_surface_item(struct np_surface *surface,
	                             int32_t origin_x, int32_t origin_y,
	                             int32_t geometry_x, int32_t geometry_y,
	                             uint32_t output_scale,
	                             struct scene_item items[NP_SCENE_MAX_LAYERS],
	                             uint32_t *count)
{
	if (surface->current_buffer && surface->has_published) {
		if (*count >= NP_SCENE_MAX_LAYERS) return false;
		struct np_surface_mapping mapping;
		if (!np_scale_resolve(surface, (uint32_t)surface->last_width,
		                      (uint32_t)surface->last_height, &mapping))
			return false;
		struct scene_item *item = &items[*count];
		memset(item, 0, sizeof(*item));
		double destination_x0 =
			(double)(origin_x - geometry_x) * (double)output_scale;
		double destination_y0 =
			(double)(origin_y - geometry_y) * (double)output_scale;
		item->surface = surface;
		item->layer = (struct np_scene_layer) {
			.destination_x0 = (float)destination_x0,
			.destination_y0 = (float)destination_y0,
			.destination_x1 = (float)(destination_x0 +
			                          mapping.logical_width * output_scale),
			.destination_y1 = (float)(destination_y0 +
			                          mapping.logical_height * output_scale),
			.source_u0 = (float)(mapping.source_x_pixels / surface->last_width),
			.source_v0 = (float)(mapping.source_y_pixels / surface->last_height),
			.source_u1 = (float)((mapping.source_x_pixels + mapping.source_width_pixels) /
			                    surface->last_width),
			.source_v1 = (float)((mapping.source_y_pixels + mapping.source_height_pixels) /
			                    surface->last_height),
			.source_x0 = (float)mapping.source_x_pixels,
			.source_y0 = (float)mapping.source_y_pixels,
			.source_x1 = (float)(mapping.source_x_pixels + mapping.source_width_pixels),
			.source_y1 = (float)(mapping.source_y_pixels + mapping.source_height_pixels),
			.opaque = surface->last_format &&
			          strcmp(surface->last_format, "bgrx8888") == 0,
		};

		struct np_gpu_buffer *gpu = np_gpu_buffer_get(surface->current_buffer);
		if (gpu) {
			struct np_gpu_scene_image source;
			if (!np_gpu_buffer_prepare_scene(gpu, &source)) return false;
			item->gpu = true;
			item->gpu_buffer = gpu;
			item->layer.image = source.image;
			item->layer.view = source.view;
			item->layer.layout = source.layout;
		} else {
			item->shm = wl_shm_buffer_get(surface->current_buffer);
			if (!item->shm) return false;
		}
		(*count)++;
	}
	return true;
}

/* The active child list is bottom-to-top. A wl_surface is itself a stacking
 * boundary: children below it are drawn first, then the surface, then children
 * above it. Firefox uses this path for some page UI instead of xdg_popup. */
static bool collect_items(struct np_surface *surface, struct np_surface *root,
	                      int32_t origin_x, int32_t origin_y,
	                      int32_t geometry_x, int32_t geometry_y,
	                      uint32_t output_scale,
	                      struct scene_item items[NP_SCENE_MAX_LAYERS],
	                      uint32_t *count)
{
	struct np_surface *child;
	wl_list_for_each(child, &surface->children, sibling_link) {
		if (!child->above_parent &&
		    !collect_items(child, root, origin_x + child->sub_x,
		                   origin_y + child->sub_y, geometry_x, geometry_y,
		                   output_scale, items, count))
			return false;
	}
	if (!collect_surface_item(surface, origin_x, origin_y, geometry_x, geometry_y,
	                         output_scale, items, count))
		return false;
	wl_list_for_each(child, &surface->children, sibling_link) {
		if (child->above_parent &&
		    !collect_items(child, root, origin_x + child->sub_x,
		                   origin_y + child->sub_y, geometry_x, geometry_y,
		                   output_scale, items, count))
			return false;
	}
	return true;
}

static unsigned char blend_channel(unsigned char source, unsigned char destination,
	                               unsigned alpha)
{
	return (unsigned char)(source +
		((unsigned)destination * (255u - alpha) + 127u) / 255u);
}

static bool draw_shm_item(struct np_blob *target, uint32_t width, uint32_t height,
	                      const struct scene_item *item)
{
	if (!item->shm || !target->data || !np_gpu_prepare_host_write(&target->vk))
		return false;
	uint32_t format = wl_shm_buffer_get_format(item->shm);
	if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888)
		return false;
	wl_shm_buffer_begin_access(item->shm);
	const unsigned char *source = wl_shm_buffer_get_data(item->shm);
	int32_t source_width = wl_shm_buffer_get_width(item->shm);
	int32_t source_height = wl_shm_buffer_get_height(item->shm);
	int32_t source_stride = wl_shm_buffer_get_stride(item->shm);
	const struct np_scene_layer *layer = &item->layer;
	int32_t x0 = (int32_t)floorf(fmaxf(layer->destination_x0, 0.0f));
	int32_t y0 = (int32_t)floorf(fmaxf(layer->destination_y0, 0.0f));
	int32_t x1 = (int32_t)ceilf(fminf(layer->destination_x1, (float)width));
	int32_t y1 = (int32_t)ceilf(fminf(layer->destination_y1, (float)height));
	float dx = layer->destination_x1 - layer->destination_x0;
	float dy = layer->destination_y1 - layer->destination_y0;
	static bool traced_source;
	if (!traced_source && source) {
		uint64_t nonzero = 0;
		unsigned alpha_min = 255, alpha_max = 0;
		for (int32_t sy = 0; sy < source_height; sy++) {
			const unsigned char *pixel = source + (size_t)sy * source_stride;
			for (int32_t sx = 0; sx < source_width; sx++, pixel += 4) {
				nonzero += pixel[0] || pixel[1] || pixel[2] || pixel[3];
				if (pixel[3] < alpha_min) alpha_min = pixel[3];
				if (pixel[3] > alpha_max) alpha_max = pixel[3];
			}
		}
		fprintf(stderr,
		        "[scene] shm source=%dx%d stride=%d nonzero=%llu alpha=%u..%u "
		        "src=%.1f,%.1f..%.1f,%.1f dst=%.1f,%.1f..%.1f,%.1f clip=%d,%d..%d,%d\n",
		        source_width, source_height, source_stride,
		        (unsigned long long)nonzero, alpha_min, alpha_max,
		        layer->source_x0, layer->source_y0,
		        layer->source_x1, layer->source_y1,
		        layer->destination_x0, layer->destination_y0,
		        layer->destination_x1, layer->destination_y1,
		        x0, y0, x1, y1);
		traced_source = true;
	}
	if (!source || dx <= 0 || dy <= 0 || x1 <= x0 || y1 <= y0) {
		wl_shm_buffer_end_access(item->shm);
		return source != NULL;
	}
	for (int32_t y = y0; y < y1; y++) {
		float source_y = layer->source_y0 +
			((y + 0.5f - layer->destination_y0) / dy) *
			(layer->source_y1 - layer->source_y0);
		int32_t sy = (int32_t)floorf(source_y);
		if (sy < 0) sy = 0;
		if (sy >= source_height) sy = source_height - 1;
		unsigned char *destination = (unsigned char *)target->data +
			(size_t)y * target->vk.stride + (size_t)x0 * 4u;
		for (int32_t x = x0; x < x1; x++, destination += 4) {
			float source_x = layer->source_x0 +
				((x + 0.5f - layer->destination_x0) / dx) *
				(layer->source_x1 - layer->source_x0);
			int32_t sx = (int32_t)floorf(source_x);
			if (sx < 0) sx = 0;
			if (sx >= source_width) sx = source_width - 1;
			const unsigned char *pixel = source +
				(size_t)sy * (size_t)source_stride + (size_t)sx * 4u;
			unsigned alpha = format == WL_SHM_FORMAT_XRGB8888 ? 255u : pixel[3];
			if (alpha == 255u) {
				destination[0] = pixel[0];
				destination[1] = pixel[1];
				destination[2] = pixel[2];
				destination[3] = 255;
			} else if (alpha) {
				destination[0] = blend_channel(pixel[0], destination[0], alpha);
				destination[1] = blend_channel(pixel[1], destination[1], alpha);
				destination[2] = blend_channel(pixel[2], destination[2], alpha);
				destination[3] = blend_channel((unsigned char)alpha,
				                               destination[3], alpha);
			}
		}
	}
	wl_shm_buffer_end_access(item->shm);
	np_vk_surface_buffer_flush(&target->vk);
	return true;
}

static bool clear_target(struct np_blob *target, uint32_t width, uint32_t height)
{
	if (!target->data || !np_gpu_prepare_host_write(&target->vk)) return false;
	for (uint32_t y = 0; y < height; y++)
		memset((unsigned char *)target->data + (size_t)y * target->vk.stride,
		       0, (size_t)width * 4u);
	np_vk_surface_buffer_flush(&target->vk);
	return true;
}

static bool resolve_transfer_region(
	const struct np_scene_layer *layer, uint32_t width, uint32_t height,
	int32_t *sx0, int32_t *sy0, int32_t *sx1, int32_t *sy1,
	int32_t *dx0, int32_t *dy0, int32_t *dx1, int32_t *dy1)
{
	float destination_width = layer->destination_x1 - layer->destination_x0;
	float destination_height = layer->destination_y1 - layer->destination_y0;
	if (destination_width <= 0 || destination_height <= 0) return false;
	float cx0 = fmaxf(layer->destination_x0, 0.0f);
	float cy0 = fmaxf(layer->destination_y0, 0.0f);
	float cx1 = fminf(layer->destination_x1, (float)width);
	float cy1 = fminf(layer->destination_y1, (float)height);
	if (cx1 <= cx0 || cy1 <= cy0) return false;
	float source_scale_x =
		(layer->source_x1 - layer->source_x0) / destination_width;
	float source_scale_y =
		(layer->source_y1 - layer->source_y0) / destination_height;
	*sx0 = (int32_t)lroundf(layer->source_x0 +
		(cx0 - layer->destination_x0) * source_scale_x);
	*sy0 = (int32_t)lroundf(layer->source_y0 +
		(cy0 - layer->destination_y0) * source_scale_y);
	*sx1 = (int32_t)lroundf(layer->source_x0 +
		(cx1 - layer->destination_x0) * source_scale_x);
	*sy1 = (int32_t)lroundf(layer->source_y0 +
		(cy1 - layer->destination_y0) * source_scale_y);
	*dx0 = (int32_t)lroundf(cx0);
	*dy0 = (int32_t)lroundf(cy0);
	*dx1 = (int32_t)lroundf(cx1);
	*dy1 = (int32_t)lroundf(cy1);
	return *sx1 > *sx0 && *sy1 > *sy0 && *dx1 > *dx0 && *dy1 > *dy0;
}

static void trace_first_output(struct np_blob *target,
	                           uint32_t width, uint32_t height)
{
	static bool traced;
	if (traced || !target || !target->data ||
	    !np_vk_surface_buffer_invalidate(&target->vk)) return;
	uint64_t nonzero = 0;
	unsigned alpha_min = 255, alpha_max = 0;
	for (uint32_t y = 0; y < height; y++) {
		const unsigned char *pixel = (const unsigned char *)target->data +
			(size_t)y * target->vk.stride;
		for (uint32_t x = 0; x < width; x++, pixel += 4) {
			nonzero += pixel[0] || pixel[1] || pixel[2] || pixel[3];
			if (pixel[3] < alpha_min) alpha_min = pixel[3];
			if (pixel[3] > alpha_max) alpha_max = pixel[3];
		}
	}
	fprintf(stderr, "[scene] output res=%u nonzero=%llu/%llu alpha=%u..%u\n",
	        target->resource_id, (unsigned long long)nonzero,
	        (unsigned long long)width * height, alpha_min, alpha_max);
	traced = true;
}

bool np_scene_compose(struct np_surface *root, uint32_t presentation_id,
	                  struct np_scene_frame *frame)
{
	uint64_t perf_start = np_perf_now_ns();
	if (!root || !frame || !presentation_id ||
	    (!root->toplevel && !root->popup) || !root->has_published) {
		return false;
	}

	struct np_surface_mapping root_mapping;
	if (!np_scale_resolve(root, (uint32_t)root->last_width,
	                     (uint32_t)root->last_height, &root_mapping)) {
		if (scene_trace_enabled())
			fprintf(stderr, "[scene] scale resolve failed root=%u source=%dx%d\n",
			        root->id, root->last_width, root->last_height);
		return false;
	}
	int32_t gx = root->geometry_set ? root->geometry_x : 0;
	int32_t gy = root->geometry_set ? root->geometry_y : 0;
	int32_t gw = root->geometry_set ? root->geometry_width
	                              : (int32_t)llround(root_mapping.logical_width);
	int32_t gh = root->geometry_set ? root->geometry_height
	                              : (int32_t)llround(root_mapping.logical_height);
	uint32_t scale = root->server->output_scale > 0
		? (uint32_t)root->server->output_scale : 1u;
	if (gw <= 0 || gh <= 0 || (uint64_t)gw * scale > UINT32_MAX ||
	    (uint64_t)gh * scale > UINT32_MAX) {
		if (scene_trace_enabled())
			fprintf(stderr, "[scene] invalid output geometry root=%u %dx%d scale=%u\n",
			        root->id, gw, gh, scale);
		return false;
	}
	uint32_t width = (uint32_t)gw * scale;
	uint32_t height = (uint32_t)gh * scale;
	if (!ensure_output(root, width, height)) {
		if (scene_trace_enabled())
			fprintf(stderr, "[scene] output allocation failed root=%u %ux%u\n",
			        root->id, width, height);
		return false;
	}
	int slot = acquire_slot(&root->scene);
	if (slot < 0) {
		if (scene_trace_enabled())
			fprintf(stderr, "[scene] no free output slot root=%u present=%u\n",
			        root->id, presentation_id);
		return false;
	}

	struct np_blob *target = &root->scene.images[slot];
	struct scene_item items[NP_SCENE_MAX_LAYERS];
	uint32_t item_count = 0;
	if (!collect_items(root, root, 0, 0, gx, gy, scale, items, &item_count) ||
	    !item_count) {
		if (scene_trace_enabled())
			fprintf(stderr, "[scene] no composable items root=%u current=%p\n",
			        root->id, (void *)root->current_buffer);
		return false;
	}

	bool any_gpu = false;
	bool all_gpu = true;
	for (uint32_t i = 0; i < item_count; i++) {
		any_gpu |= items[i].gpu;
		all_gpu &= items[i].gpu;
	}
	if (scene_trace_enabled())
		fprintf(stderr, "[scene] compose root=%u target=%u %ux%u items=%u gpu=%d all=%d\n",
		        root->id, target->resource_id, width, height, item_count,
		        any_gpu, all_gpu);
	bool composed = false;
	if (all_gpu && item_count == 1) {
		int32_t sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1;
		if (resolve_transfer_region(
		        &items[0].layer, width, height,
		        &sx0, &sy0, &sx1, &sy1, &dx0, &dy0, &dx1, &dy1)) {
			/* The common root-only path overwrites every pixel CoreAnimation
			 * samples. Clearing the whole capacity image first doubles memory
			 * traffic during resize, where capacity deliberately exceeds the
			 * active frame. Preserve clearing only for a partially covered scene. */
			bool needs_clear = dx0 != 0 || dy0 != 0 ||
			                   dx1 != (int32_t)width || dy1 != (int32_t)height;
			composed = np_gpu_buffer_copy_to_output_region(
				items[0].gpu_buffer, &target->vk,
				sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, needs_clear);
		}
	} else if (all_gpu) {
		struct np_scene_layer layers[NP_SCENE_MAX_LAYERS];
		for (uint32_t i = 0; i < item_count; i++) layers[i] = items[i].layer;
		composed = np_scene_renderer_draw(
			&target->vk, width, height, layers, item_count, true);
	} else {
		/* wl_shm pixels go into the ordinary scene image. Mixed scenes
		 * preserve z order by alternating direct CPU layers with synchronous
		 * GPU runs, all against this same target. */
		composed = clear_target(target, width, height);
		for (uint32_t i = 0; composed && i < item_count;) {
			if (!items[i].gpu) {
				composed = draw_shm_item(target, width, height, &items[i]);
				i++;
				continue;
			}
			struct np_scene_layer layers[NP_SCENE_MAX_LAYERS];
			uint32_t count = 0;
			while (i < item_count && items[i].gpu)
				layers[count++] = items[i++].layer;
			composed = np_scene_renderer_draw(
				&target->vk, width, height, layers, count, false);
		}
	}
	if (!composed) {
		fprintf(stderr, "[scene] compose failed root=%u target=%u\n",
		        root->id, target->resource_id);
		return false;
	}
	trace_first_output(target, width, height);
	root->scene.presentation_id[slot] = presentation_id;

	*frame = (struct np_scene_frame) {
		.resource_id = target->resource_id,
		.width = width,
		.height = height,
		.stride = target->vk.stride,
		.scale = scale,
		.geometry_x = gx,
		.geometry_y = gy,
		.geometry_width = gw,
		.geometry_height = gh,
		.slot = slot,
		.gpu = any_gpu,
	};
	np_perf_record(NP_PERF_SCENE_COMPOSE, np_perf_now_ns() - perf_start);
	return true;
}

void np_scene_presented(struct np_surface *root, uint32_t presentation_id)
{
	if (!root || !presentation_id) return;
	if (root->scene.presentation_id[0] == presentation_id)
		root->scene.presentation_id[0] = 0;
	struct np_retired_scene_output **slot = &root->retired_scenes;
	while (*slot) {
		struct np_retired_scene_output *retired = *slot;
		if (retired->output.presentation_id[0] == presentation_id)
			retired->output.presentation_id[0] = 0;
		if (!retired->output.presentation_id[0]) {
			*slot = retired->next;
			np_blob_destroy(root->server->drm_fd,
			                &retired->output.images[0]);
			free(retired);
			continue;
		}
		slot = &retired->next;
	}
}

void np_scene_destroy(struct np_surface *surface)
{
	if (!surface) return;
	np_blob_destroy(surface->server->drm_fd, &surface->scene.images[0]);
	while (surface->retired_scenes) {
		struct np_retired_scene_output *retired = surface->retired_scenes;
		surface->retired_scenes = retired->next;
		np_blob_destroy(surface->server->drm_fd,
		                &retired->output.images[0]);
		free(retired);
	}
	memset(&surface->scene, 0, sizeof(surface->scene));
}

#endif
