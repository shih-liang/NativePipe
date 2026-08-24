#include "scene.h"

#include "compositor_internal.h"
#include "dmabuf.h"
#include "scale.h"
#include "shm_texture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

bool np_scene_build(struct np_surface *root, uint32_t presentation_id,
	                struct np_scene_packet *packet)
{
	(void)root; (void)presentation_id; (void)packet;
	return false;
}
bool np_scene_hold_current(struct np_surface *surface, uint32_t presentation_id)
{
	(void)surface; (void)presentation_id;
	return false;
}
void np_scene_presented(struct np_surface *root, uint32_t presentation_id)
{
	(void)root; (void)presentation_id;
}
void np_scene_discard_presentations(struct np_surface *surface) { (void)surface; }
void np_scene_destroy(struct np_surface *surface) { (void)surface; }

#else

#define NP_SCENE_MAGIC "NPSN"
#define NP_SCENE_VERSION 1u
#define NP_SCENE_HEADER_SIZE 56u
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

static bool collect_surface(
	struct np_surface *surface, int32_t origin_x, int32_t origin_y,
	int32_t geometry_x, int32_t geometry_y, uint32_t output_scale,
	uint32_t output_width, uint32_t output_height,
	struct np_scene_item items[NP_SCENE_MAX_LAYERS], uint32_t *count)
{
	if (!surface->has_published ||
	    (!surface->current_gpu && !surface->current_shm))
		return true;
	if (*count >= NP_SCENE_MAX_LAYERS) return false;

	struct np_surface_mapping mapping;
	if (!np_scale_resolve(surface, (uint32_t)surface->last_width,
	                     (uint32_t)surface->last_height, &mapping))
		return false;
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

	float x = (float)((origin_x + surface->buffer_offset_x - geometry_x) *
	                  (int32_t)output_scale);
	float y = (float)((origin_y + surface->buffer_offset_y - geometry_y) *
	                  (int32_t)output_scale);
	float width = (float)(mapping.logical_width * output_scale);
	float height = (float)(mapping.logical_height * output_scale);
	item->destination[0] = x;
	item->destination[1] = y;
	item->destination[2] = width;
	item->destination[3] = height;
	item->source[0] = (float)mapping.source_x_pixels;
	item->source[1] = (float)mapping.source_y_pixels;
	item->source[2] = (float)mapping.source_width_pixels;
	item->source[3] = (float)mapping.source_height_pixels;
	float clip_x0 = fmaxf(x, 0);
	float clip_y0 = fmaxf(y, 0);
	float clip_x1 = fminf(x + width, (float)output_width);
	float clip_y1 = fminf(y + height, (float)output_height);
	if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0) {
		(*count)--;
		return true;
	}
	item->clip[0] = clip_x0;
	item->clip[1] = clip_y0;
	item->clip[2] = clip_x1 - clip_x0;
	item->clip[3] = clip_y1 - clip_y0;

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
	return item->resource_id != 0;
}

static bool collect_tree(
	struct np_surface *surface, int32_t origin_x, int32_t origin_y,
	int32_t geometry_x, int32_t geometry_y, uint32_t output_scale,
	uint32_t output_width, uint32_t output_height,
	struct np_scene_item items[NP_SCENE_MAX_LAYERS], uint32_t *count)
{
	struct np_surface *child;
	wl_list_for_each(child, &surface->children, sibling_link) {
		if (!child->above_parent &&
		    !collect_tree(child, origin_x + child->sub_x,
		                  origin_y + child->sub_y, geometry_x, geometry_y,
		                  output_scale, output_width, output_height, items, count))
			return false;
	}
	if (!collect_surface(surface, origin_x, origin_y, geometry_x, geometry_y,
	                     output_scale, output_width, output_height, items, count))
		return false;
	wl_list_for_each(child, &surface->children, sibling_link) {
		if (child->above_parent &&
		    !collect_tree(child, origin_x + child->sub_x,
		                  origin_y + child->sub_y, geometry_x, geometry_y,
		                  output_scale, output_width, output_height, items, count))
			return false;
	}
	return true;
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
}

bool np_scene_hold_current(struct np_surface *surface, uint32_t presentation_id)
{
	if (!surface || !presentation_id ||
	    (!surface->current_gpu && !surface->current_shm))
		return false;
	struct np_scene_presentation *presentation = calloc(1, sizeof(*presentation));
	if (!presentation) return false;
	wl_list_init(&presentation->link);
	presentation->id = presentation_id;
	struct np_scene_reference reference;
	if (surface->current_gpu) {
		if (!np_gpu_buffer_begin_host_read(surface->current_gpu)) {
			free(presentation);
			return false;
		}
		reference.kind = NP_SCENE_GPU;
		reference.gpu = surface->current_gpu;
	} else {
		np_shm_texture_begin_host_read(surface->current_shm);
		reference.kind = NP_SCENE_SHM;
		reference.shm = surface->current_shm;
	}
	presentation->references[0] = reference;
	presentation->reference_count = 1;
	wl_list_insert(&surface->scene_presentations, &presentation->link);
	return true;
}

bool np_scene_build(struct np_surface *root, uint32_t presentation_id,
	                struct np_scene_packet *packet)
{
	if (!root || !packet || !presentation_id || !root->has_published)
		return false;
	memset(packet, 0, sizeof(*packet));

	struct np_surface_mapping root_mapping;
	if (!np_scale_resolve(root, (uint32_t)root->last_width,
	                     (uint32_t)root->last_height, &root_mapping))
		return false;
	int32_t gx = root->geometry_set ? root->geometry_x : 0;
	int32_t gy = root->geometry_set ? root->geometry_y : 0;
	int32_t gw = root->geometry_set ? root->geometry_width
	                              : (int32_t)llround(root_mapping.logical_width);
	int32_t gh = root->geometry_set ? root->geometry_height
	                              : (int32_t)llround(root_mapping.logical_height);
	uint32_t scale = root->server->output_scale > 0
		? (uint32_t)root->server->output_scale : 1u;
	if (gw <= 0 || gh <= 0 || scale > 4 ||
	    (uint64_t)gw * scale > UINT32_MAX ||
	    (uint64_t)gh * scale > UINT32_MAX)
		return false;
	uint32_t output_width = (uint32_t)gw * scale;
	uint32_t output_height = (uint32_t)gh * scale;

	struct np_scene_item items[NP_SCENE_MAX_LAYERS];
	uint32_t count = 0;
	if (!collect_tree(root, 0, 0, gx, gy, scale,
	                  output_width, output_height, items, &count) || !count)
		return false;

	struct np_scene_presentation *presentation = calloc(1, sizeof(*presentation));
	if (!presentation) return false;
	wl_list_init(&presentation->link);
	presentation->id = presentation_id;
	for (uint32_t i = 0; i < count; i++) {
		bool ready;
		if (items[i].reference.kind == NP_SCENE_GPU)
			ready = np_gpu_buffer_begin_host_read(items[i].reference.gpu);
		else {
			np_shm_texture_begin_host_read(items[i].reference.shm);
			ready = true;
		}
		if (!ready) {
			release_references(presentation);
			free(presentation);
			return false;
		}
		presentation->references[presentation->reference_count++] = items[i].reference;
	}

	size_t size = NP_SCENE_HEADER_SIZE + (size_t)count * NP_SCENE_LAYER_SIZE;
	unsigned char *bytes = calloc(1, size);
	if (!bytes) {
		release_references(presentation);
		free(presentation);
		return false;
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
	put_u32(bytes + 52, 0);

	for (uint32_t i = 0; i < count; i++) {
		unsigned char *layer = bytes + NP_SCENE_HEADER_SIZE +
		                       (size_t)i * NP_SCENE_LAYER_SIZE;
		put_u32(layer + 0, items[i].surface->id);
		put_u32(layer + 4, items[i].resource_id);
		put_u32(layer + 8, items[i].width);
		put_u32(layer + 12, items[i].height);
		put_u32(layer + 16, items[i].stride);
		put_u16(layer + 20, items[i].format);
		put_u16(layer + 22, items[i].flags);
		put_u32(layer + 24, items[i].transform);
		for (uint32_t j = 0; j < 4; j++) {
			put_f32(layer + 32 + j * 4, items[i].destination[j]);
			put_f32(layer + 48 + j * 4, items[i].source[j]);
			put_f32(layer + 64 + j * 4, items[i].clip[j]);
		}
		put_f32(layer + 80, 1.0f);
	}

	wl_list_insert(&root->scene_presentations, &presentation->link);
	packet->data = bytes;
	packet->size = size;
	return true;
}

void np_scene_presented(struct np_surface *root, uint32_t presentation_id)
{
	if (!root || !presentation_id) return;
	struct np_scene_presentation *presentation, *tmp;
	wl_list_for_each_safe(presentation, tmp, &root->scene_presentations, link) {
		if (presentation->id != presentation_id) continue;
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
