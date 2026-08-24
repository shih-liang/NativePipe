#include "scene.h"

#include "compositor_internal.h"
#include "dmabuf.h"
#include "scale.h"
#include "shm_texture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#ifdef NP_REMOTE

struct np_surface *np_scene_root(struct np_surface *surface)
{
	while (surface && surface->parent) surface = surface->parent;
	return surface && (surface->toplevel || surface->popup) ? surface : NULL;
}
struct np_surface *np_scene_hit_test(struct np_surface *root, double x, double y,
	                                double *local_x, double *local_y)
{
	(void)x; (void)y; (void)local_x; (void)local_y;
	return root;
}

enum np_scene_build_result np_scene_build(
	struct np_surface *root, uint32_t presentation_id,
	struct np_scene_packet *packet, int *wait_fd)
{
	(void)root; (void)presentation_id; (void)packet;
	if (wait_fd) *wait_fd = -1;
	return NP_SCENE_INVALID;
}
enum np_scene_build_result np_scene_hold_current(
	struct np_surface *surface, uint32_t presentation_id, int *wait_fd)
{
	(void)surface; (void)presentation_id;
	if (wait_fd) *wait_fd = -1;
	return NP_SCENE_INVALID;
}
void np_scene_note_damage(struct np_surface *surface,
	                      const struct np_box *buffer_damage, bool full_scene)
{
	(void)surface; (void)buffer_damage; (void)full_scene;
}
void np_scene_damage_sent(struct np_surface *root) { (void)root; }
void np_scene_presented(struct np_surface *root, uint32_t presentation_id)
{
	(void)root; (void)presentation_id;
}
void np_scene_discard_presentations(struct np_surface *surface) { (void)surface; }
void np_scene_destroy(struct np_surface *surface) { (void)surface; }

#else

#define NP_SCENE_MAGIC "NPSN"
#define NP_SCENE_VERSION 2u
#define NP_SCENE_HEADER_SIZE 72u
#define NP_SCENE_LAYER_SIZE 88u
#define NP_SCENE_MAX_LAYERS 128u

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
	struct np_scene_reference references[NP_SCENE_MAX_LAYERS];
};

struct np_scene_item {
	struct np_surface *surface;
	uint32_t resource_id;
	uint32_t width, height, stride;
	uint16_t format, flags;
	uint32_t transform;
	float destination[4], source[4], clip[4];
	struct np_scene_reference reference;
};

struct np_surface *np_scene_root(struct np_surface *surface)
{
	while (surface && surface->parent) surface = surface->parent;
	return surface && (surface->toplevel || surface->popup) ? surface : NULL;
}

void np_scene_note_damage(struct np_surface *surface,
	                      const struct np_box *buffer_damage, bool full_scene)
{
	struct np_surface *root = np_scene_root(surface);
	if (!surface || !root) return;
	if (buffer_damage && buffer_damage->width > 0 && buffer_damage->height > 0)
		np_box_union(&surface->scene_damage,
		             buffer_damage->x, buffer_damage->y,
		             buffer_damage->width, buffer_damage->height);
	if (full_scene) root->scene_full_damage = true;
}

void np_scene_damage_sent(struct np_surface *root)
{
	if (!root) return;
	root->scene_full_damage = false;
	struct np_surface *surface;
	wl_list_for_each(surface, &root->server->surfaces, link) {
		if (np_scene_root(surface) == root) np_box_clear(&surface->scene_damage);
	}
}

static struct np_surface *hit_tree(
	struct np_surface *surface, double root_x, double root_y,
	double origin_x, double origin_y, double *local_x, double *local_y)
{
	struct np_surface *child;
	/* children is bottom-to-top, so hit testing walks it backwards. */
	wl_list_for_each_reverse(child, &surface->children, sibling_link) {
		if (!child->above_parent) continue;
		struct np_surface *hit = hit_tree(
			child, root_x, root_y, origin_x + child->sub_x,
			origin_y + child->sub_y, local_x, local_y);
		if (hit) return hit;
	}

	if (surface->has_published && surface->last_width > 0 && surface->last_height > 0) {
		struct np_surface_mapping mapping;
		if (np_scale_resolve(surface, (uint32_t)surface->last_width,
		                     (uint32_t)surface->last_height, &mapping)) {
			double sx = root_x - origin_x;
			double sy = root_y - origin_y;
			bool inside = sx >= surface->buffer_offset_x &&
			              sy >= surface->buffer_offset_y &&
			              sx < surface->buffer_offset_x + mapping.logical_width &&
			              sy < surface->buffer_offset_y + mapping.logical_height;
			if (inside && (!surface->input_region_set ||
			               np_region_contains(&surface->input_region, sx, sy))) {
				if (local_x) *local_x = sx;
				if (local_y) *local_y = sy;
				return surface;
			}
		}
	}

	wl_list_for_each_reverse(child, &surface->children, sibling_link) {
		if (child->above_parent) continue;
		struct np_surface *hit = hit_tree(
			child, root_x, root_y, origin_x + child->sub_x,
			origin_y + child->sub_y, local_x, local_y);
		if (hit) return hit;
	}
	return NULL;
}

struct np_surface *np_scene_hit_test(struct np_surface *root, double x, double y,
	                                double *local_x, double *local_y)
{
	if (!root) return NULL;
	/* The NSWindow content origin is xdg_window_geometry's origin, while the
	 * surface tree origin remains (0,0). */
	double root_x = x + (root->geometry_set ? root->geometry_x : 0);
	double root_y = y + (root->geometry_set ? root->geometry_y : 0);
	return hit_tree(root, root_x, root_y, 0, 0, local_x, local_y);
}

static void source_to_destination(uint32_t transform, double u, double v,
	                              double *destination_u, double *destination_v)
{
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_90:
		*destination_u = 1.0 - v; *destination_v = u; break;
	case WL_OUTPUT_TRANSFORM_180:
		*destination_u = 1.0 - u; *destination_v = 1.0 - v; break;
	case WL_OUTPUT_TRANSFORM_270:
		*destination_u = v; *destination_v = 1.0 - u; break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		*destination_u = 1.0 - u; *destination_v = v; break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		*destination_u = v; *destination_v = u; break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		*destination_u = u; *destination_v = 1.0 - v; break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		*destination_u = 1.0 - v; *destination_v = 1.0 - u; break;
	default:
		*destination_u = u; *destination_v = v; break;
	}
}

static void collect_item_damage(const struct np_scene_item *item,
	                            struct np_box *scene_damage)
{
	const struct np_box *damage = &item->surface->scene_damage;
	if (!scene_damage || damage->width <= 0 || damage->height <= 0 ||
	    item->source[2] <= 0 || item->source[3] <= 0) return;
	double sx0 = fmax(item->source[0], (double)damage->x);
	double sy0 = fmax(item->source[1], (double)damage->y);
	double sx1 = fmin(item->source[0] + item->source[2],
	                  (double)damage->x + damage->width);
	double sy1 = fmin(item->source[1] + item->source[3],
	                  (double)damage->y + damage->height);
	if (sx1 <= sx0 || sy1 <= sy0) return;
	double min_u = 1.0, min_v = 1.0, max_u = 0.0, max_v = 0.0;
	for (int corner = 0; corner < 4; corner++) {
		double source_u = (((corner & 1) ? sx1 : sx0) - item->source[0]) /
		                  item->source[2];
		double source_v = (((corner & 2) ? sy1 : sy0) - item->source[1]) /
		                  item->source[3];
		double u, v;
		source_to_destination(item->transform, source_u, source_v, &u, &v);
		if (u < min_u) min_u = u;
		if (v < min_v) min_v = v;
		if (u > max_u) max_u = u;
		if (v > max_v) max_v = v;
	}
	double x0 = fmax(item->clip[0], item->destination[0] + min_u * item->destination[2]);
	double y0 = fmax(item->clip[1], item->destination[1] + min_v * item->destination[3]);
	double x1 = fmin(item->clip[0] + item->clip[2],
	                  item->destination[0] + max_u * item->destination[2]);
	double y1 = fmin(item->clip[1] + item->clip[3],
	                  item->destination[1] + max_v * item->destination[3]);
	if (x1 <= x0 || y1 <= y0) return;
	int64_t left = (int64_t)floor(x0);
	int64_t top = (int64_t)floor(y0);
	int64_t right = (int64_t)ceil(x1);
	int64_t bottom = (int64_t)ceil(y1);
	np_box_union(scene_damage, left, top, right - left, bottom - top);
}

static enum np_scene_build_result collect_surface(
	struct np_surface *surface, int64_t origin_x, int64_t origin_y,
	int32_t geometry_x, int32_t geometry_y, uint32_t output_scale,
	uint32_t output_width, uint32_t output_height,
	struct np_scene_item items[NP_SCENE_MAX_LAYERS], uint32_t *count,
	struct np_box *scene_damage)
{
	if (!surface->has_published ||
	    (!surface->current_gpu && !surface->current_shm))
		return NP_SCENE_READY;
	if (*count >= NP_SCENE_MAX_LAYERS) return NP_SCENE_INVALID;

	struct np_surface_mapping mapping;
	if (!np_scale_resolve(surface, (uint32_t)surface->last_width,
	                     (uint32_t)surface->last_height, &mapping))
		return NP_SCENE_INVALID;
	double x_value = ((double)origin_x + surface->buffer_offset_x - geometry_x) *
	                 output_scale;
	double y_value = ((double)origin_y + surface->buffer_offset_y - geometry_y) *
	                 output_scale;
	double width_value = mapping.logical_width * output_scale;
	double height_value = mapping.logical_height * output_scale;
	double clip_x0 = fmax(x_value, 0.0);
	double clip_y0 = fmax(y_value, 0.0);
	double clip_x1 = fmin(x_value + width_value, output_width);
	double clip_y1 = fmin(y_value + height_value, output_height);
	if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0) return NP_SCENE_READY;

	struct np_scene_item *item = &items[(*count)++];
	memset(item, 0, sizeof(*item));
	item->surface = surface;
	item->width = (uint32_t)surface->last_width;
	item->height = (uint32_t)surface->last_height;
	item->stride = surface->last_stride;
	item->format = surface->last_format &&
	               strcmp(surface->last_format, "bgrx8888") == 0 ? 2u :
	               surface->last_format &&
	               strcmp(surface->last_format, "rgba8888") == 0 ? 3u : 1u;
	item->flags = item->format == 2u ? 1u : 0u;
	item->transform = (uint32_t)surface->transform;

	float x = (float)x_value;
	float y = (float)y_value;
	float width = (float)width_value;
	float height = (float)height_value;
	item->destination[0] = x;
	item->destination[1] = y;
	item->destination[2] = width;
	item->destination[3] = height;
	item->source[0] = (float)mapping.source_x_pixels;
	item->source[1] = (float)mapping.source_y_pixels;
	item->source[2] = (float)mapping.source_width_pixels;
	item->source[3] = (float)mapping.source_height_pixels;
	item->clip[0] = (float)clip_x0;
	item->clip[1] = (float)clip_y0;
	item->clip[2] = (float)(clip_x1 - clip_x0);
	item->clip[3] = (float)(clip_y1 - clip_y0);

	if (surface->current_gpu) {
		item->resource_id = surface->current_gpu->resource_id;
		item->reference.kind = NP_SCENE_GPU;
		item->reference.gpu = surface->current_gpu;
	} else {
		item->resource_id = surface->current_shm->image.resource_id;
		item->stride = surface->current_shm->stride;
		item->reference.kind = NP_SCENE_SHM;
		item->reference.shm = surface->current_shm;
	}
	collect_item_damage(item, scene_damage);
	return item->resource_id != 0 ? NP_SCENE_READY : NP_SCENE_INVALID;
}

static enum np_scene_build_result collect_tree(
	struct np_surface *surface, int64_t origin_x, int64_t origin_y,
	int32_t geometry_x, int32_t geometry_y, uint32_t output_scale,
	uint32_t output_width, uint32_t output_height,
	struct np_scene_item items[NP_SCENE_MAX_LAYERS], uint32_t *count,
	uint32_t depth, struct np_box *scene_damage)
{
	if (depth >= NP_SCENE_MAX_LAYERS) return NP_SCENE_INVALID;
	struct np_surface *child;
	wl_list_for_each(child, &surface->children, sibling_link) {
		if (!child->above_parent) {
			enum np_scene_build_result result = collect_tree(
				child, origin_x + child->sub_x, origin_y + child->sub_y,
					geometry_x, geometry_y, output_scale, output_width, output_height,
					items, count, depth + 1, scene_damage);
			if (result != NP_SCENE_READY) return result;
		}
	}
	enum np_scene_build_result result = collect_surface(
		surface, origin_x, origin_y, geometry_x, geometry_y,
		output_scale, output_width, output_height, items, count, scene_damage);
	if (result != NP_SCENE_READY) return result;
	wl_list_for_each(child, &surface->children, sibling_link) {
		if (child->above_parent) {
			result = collect_tree(
				child, origin_x + child->sub_x, origin_y + child->sub_y,
					geometry_x, geometry_y, output_scale, output_width, output_height,
					items, count, depth + 1, scene_damage);
			if (result != NP_SCENE_READY) return result;
		}
	}
	return NP_SCENE_READY;
}

static void put_u16(unsigned char *p, uint16_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
}
static void put_u32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}
static void put_i32(unsigned char *p, int32_t value) { put_u32(p, (uint32_t)value); }
static void put_f32(unsigned char *p, float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	put_u32(p, bits);
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

/* A texture may appear more than once in one subsurface tree. Hold and release
 * each imported object once per presentation, independent of layer count. */
static bool retain_reference(struct np_scene_presentation *presentation,
	                         struct np_scene_reference reference)
{
	for (uint32_t i = 0; i < presentation->reference_count; i++) {
		if (same_reference(&presentation->references[i], &reference)) return true;
	}
	if (presentation->reference_count >= NP_SCENE_MAX_LAYERS) return false;
	if (reference.kind == NP_SCENE_GPU) {
		if (!np_gpu_buffer_acquire_host_read(reference.gpu)) return false;
	} else {
		np_shm_texture_begin_host_read(reference.shm);
	}
	presentation->references[presentation->reference_count++] = reference;
	return true;
}

enum np_scene_build_result np_scene_hold_current(
	struct np_surface *surface, uint32_t presentation_id, int *wait_fd)
{
	if (wait_fd) *wait_fd = -1;
	if (!surface || !presentation_id ||
	    (!surface->current_gpu && !surface->current_shm))
		return NP_SCENE_INVALID;
	struct np_scene_presentation *presentation = calloc(1, sizeof(*presentation));
	if (!presentation) return NP_SCENE_NO_MEMORY;
	wl_list_init(&presentation->link);
	presentation->id = presentation_id;
	struct np_scene_reference reference;
	if (surface->current_gpu) {
		reference.kind = NP_SCENE_GPU;
		reference.gpu = surface->current_gpu;
	} else {
		reference.kind = NP_SCENE_SHM;
		reference.shm = surface->current_shm;
	}
	if (!retain_reference(presentation, reference)) {
		free(presentation);
		return NP_SCENE_INVALID;
	}
	wl_list_insert(&surface->scene_presentations, &presentation->link);
	return NP_SCENE_READY;
}

enum np_scene_build_result np_scene_build(
	struct np_surface *root, uint32_t presentation_id,
	struct np_scene_packet *packet, int *wait_fd)
{
	if (wait_fd) *wait_fd = -1;
	if (!root || !packet || !presentation_id || !root->has_published)
		return NP_SCENE_INVALID;
	memset(packet, 0, sizeof(*packet));

	struct np_surface_mapping root_mapping;
	if (!np_scale_resolve(root, (uint32_t)root->last_width,
	                     (uint32_t)root->last_height, &root_mapping))
		return NP_SCENE_INVALID;
	int32_t gx = root->geometry_set ? root->geometry_x : 0;
	int32_t gy = root->geometry_set ? root->geometry_y : 0;
	int32_t gw = root->geometry_set ? root->geometry_width
	                              : (int32_t)llround(root_mapping.logical_width);
	int32_t gh = root->geometry_set ? root->geometry_height
	                              : (int32_t)llround(root_mapping.logical_height);
	uint32_t scale = root->preferred_scale > 0
		? (uint32_t)root->preferred_scale : 1u;
	if (gw <= 0 || gh <= 0 || scale > 4 ||
	    (uint64_t)gw * scale > INT32_MAX ||
	    (uint64_t)gh * scale > INT32_MAX)
		return NP_SCENE_INVALID;
	uint32_t output_width = (uint32_t)gw * scale;
	uint32_t output_height = (uint32_t)gh * scale;

	struct np_scene_item items[NP_SCENE_MAX_LAYERS];
	uint32_t count = 0;
	struct np_box scene_damage;
	np_box_clear(&scene_damage);
	enum np_scene_build_result collected = collect_tree(
		root, 0, 0, gx, gy, scale, output_width, output_height,
		items, &count, 0, &scene_damage);
	if (collected != NP_SCENE_READY || !count) {
		return collected == NP_SCENE_READY ? NP_SCENE_INVALID : collected;
	}

	struct np_scene_presentation *presentation = calloc(1, sizeof(*presentation));
	if (!presentation) return NP_SCENE_NO_MEMORY;
	wl_list_init(&presentation->link);
	presentation->id = presentation_id;
	for (uint32_t i = 0; i < count; i++) {
		if (!retain_reference(presentation, items[i].reference)) {
			release_references(presentation);
			free(presentation);
			return NP_SCENE_INVALID;
		}
	}

	size_t size = NP_SCENE_HEADER_SIZE + (size_t)count * NP_SCENE_LAYER_SIZE;
	unsigned char *bytes = calloc(1, size);
	if (!bytes) {
		release_references(presentation);
		free(presentation);
		return NP_SCENE_NO_MEMORY;
	}
	memcpy(bytes, NP_SCENE_MAGIC, 4);
	put_u16(bytes + 4, NP_SCENE_VERSION);
	put_u16(bytes + 6, NP_SCENE_HEADER_SIZE);
	put_u32(bytes + 8, (uint32_t)size);
	put_u32(bytes + 12, root->id);
	put_u32(bytes + 16, presentation_id);
	put_u32(bytes + 20, output_width);
	put_u32(bytes + 24, output_height);
	put_u32(bytes + 28, scale);
	put_i32(bytes + 32, gx);
	put_i32(bytes + 36, gy);
	put_i32(bytes + 40, gw);
	put_i32(bytes + 44, gh);
	put_u32(bytes + 48, count);
	put_u32(bytes + 52, root->scene_full_damage ? 1u : 0u);
	if (root->scene_full_damage) {
		scene_damage = (struct np_box){0, 0, output_width, output_height};
	}
	put_i32(bytes + 56, (int32_t)scene_damage.x);
	put_i32(bytes + 60, (int32_t)scene_damage.y);
	put_i32(bytes + 64, (int32_t)scene_damage.width);
	put_i32(bytes + 68, (int32_t)scene_damage.height);

	for (uint32_t i = 0; i < count; i++) {
		unsigned char *layer = bytes + NP_SCENE_HEADER_SIZE +
		                       (size_t)i * NP_SCENE_LAYER_SIZE;
		struct np_scene_item *item = &items[i];
		put_u32(layer + 0, item->surface->id);
		put_u32(layer + 4, item->resource_id);
		put_u32(layer + 8, item->width);
		put_u32(layer + 12, item->height);
		put_u32(layer + 16, item->stride);
		put_u16(layer + 20, item->format);
		put_u16(layer + 22, item->flags);
		put_u32(layer + 24, item->transform);
		for (uint32_t j = 0; j < 4; j++) {
			put_f32(layer + 32 + j * 4, item->destination[j]);
			put_f32(layer + 48 + j * 4, item->source[j]);
			put_f32(layer + 64 + j * 4, item->clip[j]);
		}
		put_f32(layer + 80, 1.0f);
	}

	wl_list_insert(&root->scene_presentations, &presentation->link);
	packet->data = bytes;
	packet->size = size;
	return NP_SCENE_READY;
}

void np_scene_presented(struct np_surface *root, uint32_t presentation_id)
{
	if (!root || !presentation_id) return;
	struct np_scene_presentation *presentation, *tmp;
	wl_list_for_each_safe(presentation, tmp, &root->scene_presentations, link) {
		if (presentation->id != presentation_id) continue;
		if (np_trace_enabled())
			fprintf(stderr,
			        "[scene] release surface=%u present=%u references=%u\n",
			        root->id, presentation_id, presentation->reference_count);
		wl_list_remove(&presentation->link);
		release_references(presentation);
		free(presentation);
		return;
	}
}

void np_scene_discard_presentations(struct np_surface *surface)
{
	if (!surface) return;
	struct np_scene_presentation *presentation, *tmp;
	wl_list_for_each_safe(presentation, tmp, &surface->scene_presentations, link) {
		wl_list_remove(&presentation->link);
		release_references(presentation);
		free(presentation);
	}
}

void np_scene_destroy(struct np_surface *surface)
{
	np_scene_discard_presentations(surface);
}

#endif
