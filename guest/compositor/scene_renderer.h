#ifndef NP_SCENE_RENDERER_H
#define NP_SCENE_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "vk_context.h"
#include "vk_surface_buffer.h"

struct np_scene_layer {
	VkImage image;
	VkImageView view;
	VkImageLayout *layout;
	float destination_x0;
	float destination_y0;
	float destination_x1;
	float destination_y1;
	float source_u0;
	float source_v0;
	float source_u1;
	float source_v1;
	float source_x0;
	float source_y0;
	float source_x1;
	float source_y1;
	bool opaque;
};

bool np_scene_renderer_init(const struct np_vk_context *context);
void np_scene_renderer_finish(void);
bool np_scene_renderer_draw(struct np_vk_surface_buffer *output,
	                        uint32_t width, uint32_t height,
	                        const struct np_scene_layer *layers,
	                        uint32_t layer_count, bool clear);

#endif
