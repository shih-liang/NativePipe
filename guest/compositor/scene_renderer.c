#include "scene_renderer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

extern const unsigned char _binary_shaders_scene_vert_spv_start[];
extern const unsigned char _binary_shaders_scene_vert_spv_end[];
extern const unsigned char _binary_shaders_scene_frag_spv_start[];
extern const unsigned char _binary_shaders_scene_frag_spv_end[];

#define NP_SCENE_MAX_LAYERS 128u

struct scene_push {
	float destination[4];
	float source[4];
	uint32_t opaque;
};

struct scene_renderer {
	VkDevice device;
	VkQueue queue;
	uint32_t queue_family;
	VkCommandPool command_pool;
	VkCommandBuffer command;
	VkFence fence;
	VkRenderPass clear_render_pass;
	VkRenderPass load_render_pass;
	VkDescriptorSetLayout descriptor_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkSampler sampler;
	VkDescriptorPool descriptor_pool;
};

static struct scene_renderer renderer;

struct transfer_layer {
	VkOffset3D source[2];
	VkOffset3D destination[2];
	bool copy;
};

static bool near_integer(float value, int32_t *integer)
{
	double rounded = round(value);
	if (fabs((double)value - rounded) > 0.001 ||
	    rounded < INT32_MIN || rounded > INT32_MAX)
		return false;
	*integer = (int32_t)rounded;
	return true;
}

static bool prepare_transfer_layer(const struct np_scene_layer *layer,
	                               uint32_t width, uint32_t height,
	                               struct transfer_layer *transfer)
{
	float dx0 = layer->destination_x0;
	float dy0 = layer->destination_y0;
	float dx1 = layer->destination_x1;
	float dy1 = layer->destination_y1;
	if (dx1 <= dx0 || dy1 <= dy0) return false;
	float cx0 = fmaxf(dx0, 0.0f), cy0 = fmaxf(dy0, 0.0f);
	float cx1 = fminf(dx1, (float)width), cy1 = fminf(dy1, (float)height);
	if (cx1 <= cx0 || cy1 <= cy0) return false;
	float sx_scale = (layer->source_x1 - layer->source_x0) / (dx1 - dx0);
	float sy_scale = (layer->source_y1 - layer->source_y0) / (dy1 - dy0);
	float sx0 = layer->source_x0 + (cx0 - dx0) * sx_scale;
	float sy0 = layer->source_y0 + (cy0 - dy0) * sy_scale;
	float sx1 = layer->source_x0 + (cx1 - dx0) * sx_scale;
	float sy1 = layer->source_y0 + (cy1 - dy0) * sy_scale;
	int32_t values[8];
	float floats[8] = {sx0, sy0, sx1, sy1, cx0, cy0, cx1, cy1};
	for (int i = 0; i < 8; i++)
		if (!near_integer(floats[i], &values[i])) return false;
	transfer->source[0] = (VkOffset3D){values[0], values[1], 0};
	transfer->source[1] = (VkOffset3D){values[2], values[3], 1};
	transfer->destination[0] = (VkOffset3D){values[4], values[5], 0};
	transfer->destination[1] = (VkOffset3D){values[6], values[7], 1};
	transfer->copy =
		(values[2] - values[0]) == (values[6] - values[4]) &&
		(values[3] - values[1]) == (values[7] - values[5]);
	return true;
}

static bool draw_transfer_fast_path(struct np_vk_surface_buffer *output,
	                                uint32_t width, uint32_t height,
	                                const struct np_scene_layer *layers,
	                                uint32_t layer_count)
{
	struct transfer_layer transfers[NP_SCENE_MAX_LAYERS];
	for (uint32_t i = 0; i < layer_count; i++) {
		if (!layers[i].image || !layers[i].layout) return false;
		/* The root is copied over transparent output. Every later layer must be
		 * explicitly opaque; copy/blit cannot implement source-over alpha. */
		if (i != 0 && !layers[i].opaque) return false;
		if (!prepare_transfer_layer(&layers[i], width, height, &transfers[i]))
			return false;
	}
	if (vkWaitForFences(renderer.device, 1, &renderer.fence,
	                    VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
	    vkResetFences(renderer.device, 1, &renderer.fence) != VK_SUCCESS ||
	    vkResetCommandBuffer(renderer.command, 0) != VK_SUCCESS)
		return false;
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (vkBeginCommandBuffer(renderer.command, &begin) != VK_SUCCESS)
		return false;

	VkImageMemoryBarrier barriers[NP_SCENE_MAX_LAYERS + 1];
	for (uint32_t i = 0; i < layer_count; i++) {
		barriers[i] = (VkImageMemoryBarrier) {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			/* dma-buf implicit synchronization was completed before scene
			 * collection.  The producer belongs to another Vulkan context, so
			 * this queue must not claim a HOST_WRITE or prior local access. */
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.oldLayout = *layers[i].layout,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = layers[i].image,
			.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			                      .levelCount = 1, .layerCount = 1 },
		};
		*layers[i].layout = VK_IMAGE_LAYOUT_GENERAL;
	}
	barriers[layer_count] = (VkImageMemoryBarrier) {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = output->host_dirty
			? VK_ACCESS_HOST_WRITE_BIT : VK_ACCESS_MEMORY_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = output->layout,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = output->image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		                      .levelCount = 1, .layerCount = 1 },
	};
	vkCmdPipelineBarrier(renderer.command,
	                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT |
	                     VK_PIPELINE_STAGE_HOST_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
	                     0, NULL, 0, NULL, layer_count + 1, barriers);

	VkClearColorValue transparent = { .float32 = {0, 0, 0, 0} };
	VkImageSubresourceRange all = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1,
	};
	vkCmdClearColorImage(renderer.command, output->image,
	                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                     &transparent, 1, &all);
	for (uint32_t i = 0; i < layer_count; i++) {
		VkImageSubresourceLayers subresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1,
		};
		if (transfers[i].copy) {
			VkImageCopy copy = {
				.srcSubresource = subresource,
				.srcOffset = transfers[i].source[0],
				.dstSubresource = subresource,
				.dstOffset = transfers[i].destination[0],
				.extent = {
					.width = (uint32_t)(transfers[i].source[1].x - transfers[i].source[0].x),
					.height = (uint32_t)(transfers[i].source[1].y - transfers[i].source[0].y),
					.depth = 1,
				},
			};
			vkCmdCopyImage(renderer.command, layers[i].image,
			               VK_IMAGE_LAYOUT_GENERAL,
			               output->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			               1, &copy);
		} else {
			VkImageBlit blit = {
				.srcSubresource = subresource,
				.srcOffsets = { transfers[i].source[0], transfers[i].source[1] },
				.dstSubresource = subresource,
				.dstOffsets = { transfers[i].destination[0], transfers[i].destination[1] },
			};
			vkCmdBlitImage(renderer.command, layers[i].image,
			               VK_IMAGE_LAYOUT_GENERAL,
			               output->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			               1, &blit, VK_FILTER_LINEAR);
		}
	}
	VkImageMemoryBarrier present = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = output->image,
		.subresourceRange = all,
	};
	vkCmdPipelineBarrier(renderer.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
	                     0, NULL, 0, NULL, 1, &present);
	if (vkEndCommandBuffer(renderer.command) != VK_SUCCESS) return false;
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &renderer.command,
	};
	bool ok = vkQueueSubmit(renderer.queue, 1, &submit, renderer.fence) == VK_SUCCESS &&
	          vkWaitForFences(renderer.device, 1, &renderer.fence,
	                          VK_TRUE, UINT64_MAX) == VK_SUCCESS;
	if (ok) {
		output->layout = VK_IMAGE_LAYOUT_GENERAL;
		output->host_dirty = false;
	}
	return ok;
}

static bool can_transfer_fast_path(const struct np_scene_layer *layers,
	                               uint32_t layer_count,
	                               uint32_t width, uint32_t height)
{
	struct transfer_layer transfer;
	/* The raw transfer is the single-layer window fast path. XRGB uses the
	 * shader so its undefined X byte is forced to alpha=1. */
	return layer_count == 1 && !layers[0].opaque &&
	       prepare_transfer_layer(&layers[0], width, height, &transfer);
}

static VkShaderModule make_shader(const unsigned char *start,
	                              const unsigned char *end)
{
	VkShaderModuleCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = (size_t)(end - start),
		.pCode = (const uint32_t *)start,
	};
	VkShaderModule module = VK_NULL_HANDLE;
	return vkCreateShaderModule(renderer.device, &info, NULL, &module) == VK_SUCCESS
		? module : VK_NULL_HANDLE;
}

bool np_scene_renderer_init(const struct np_vk_context *context)
{
	if (!context || !context->device || !context->graphics_queue)
		return false;
	memset(&renderer, 0, sizeof(renderer));
	renderer.device = context->device;
	renderer.queue = context->graphics_queue;
	renderer.queue_family = context->graphics_queue_family;

	VkCommandPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = renderer.queue_family,
	};
	if (vkCreateCommandPool(renderer.device, &pool, NULL,
	                        &renderer.command_pool) != VK_SUCCESS)
		goto fail;
	VkCommandBufferAllocateInfo command = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = renderer.command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (vkAllocateCommandBuffers(renderer.device, &command,
	                             &renderer.command) != VK_SUCCESS)
		goto fail;
	VkFenceCreateInfo fence = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};
	if (vkCreateFence(renderer.device, &fence, NULL, &renderer.fence) != VK_SUCCESS)
		goto fail;

	VkAttachmentDescription clear_attachment = {
		.format = VK_FORMAT_B8G8R8A8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.finalLayout = VK_IMAGE_LAYOUT_GENERAL,
	};
	VkAttachmentReference color = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
	};
	VkRenderPassCreateInfo render_pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &clear_attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	if (vkCreateRenderPass(renderer.device, &render_pass, NULL,
	                       &renderer.clear_render_pass) != VK_SUCCESS)
		goto fail;
	VkAttachmentDescription load_attachment = clear_attachment;
	load_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	render_pass.pAttachments = &load_attachment;
	if (vkCreateRenderPass(renderer.device, &render_pass, NULL,
	                       &renderer.load_render_pass) != VK_SUCCESS)
		goto fail;

	VkDescriptorSetLayoutBinding binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo descriptor_layout = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	if (vkCreateDescriptorSetLayout(renderer.device, &descriptor_layout, NULL,
	                                &renderer.descriptor_layout) != VK_SUCCESS)
		goto fail;
	VkPushConstantRange push = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(struct scene_push),
	};
	VkPipelineLayoutCreateInfo pipeline_layout = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &renderer.descriptor_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push,
	};
	if (vkCreatePipelineLayout(renderer.device, &pipeline_layout, NULL,
	                           &renderer.pipeline_layout) != VK_SUCCESS)
		goto fail;

	VkShaderModule vertex = make_shader(_binary_shaders_scene_vert_spv_start,
	                                    _binary_shaders_scene_vert_spv_end);
	VkShaderModule fragment = make_shader(_binary_shaders_scene_frag_spv_start,
	                                      _binary_shaders_scene_frag_spv_end);
	if (!vertex || !fragment) {
		if (vertex) vkDestroyShaderModule(renderer.device, vertex, NULL);
		if (fragment) vkDestroyShaderModule(renderer.device, fragment, NULL);
		goto fail;
	}
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo viewport = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState blend_attachment = {
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend_attachment,
	};
	VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dynamic_states,
	};
	VkGraphicsPipelineCreateInfo pipeline = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &assembly,
		.pViewportState = &viewport,
		.pRasterizationState = &raster,
		.pMultisampleState = &multisample,
		.pColorBlendState = &blend,
		.pDynamicState = &dynamic,
		.layout = renderer.pipeline_layout,
		.renderPass = renderer.clear_render_pass,
		.subpass = 0,
	};
	VkResult pipeline_result = vkCreateGraphicsPipelines(
		renderer.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &renderer.pipeline);
	vkDestroyShaderModule(renderer.device, vertex, NULL);
	vkDestroyShaderModule(renderer.device, fragment, NULL);
	if (pipeline_result != VK_SUCCESS)
		goto fail;

	VkSamplerCreateInfo sampler = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxLod = 0.0f,
	};
	if (vkCreateSampler(renderer.device, &sampler, NULL, &renderer.sampler) != VK_SUCCESS)
		goto fail;
	VkDescriptorPoolSize pool_size = {
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = NP_SCENE_MAX_LAYERS,
	};
	VkDescriptorPoolCreateInfo descriptor_pool = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = NP_SCENE_MAX_LAYERS,
		.poolSizeCount = 1,
		.pPoolSizes = &pool_size,
	};
	if (vkCreateDescriptorPool(renderer.device, &descriptor_pool, NULL,
	                           &renderer.descriptor_pool) != VK_SUCCESS)
		goto fail;
	fprintf(stderr, "[scene] Vulkan window compositor initialized\n");
	return true;

fail:
	np_scene_renderer_finish();
	fprintf(stderr, "[scene] Vulkan window compositor initialization failed\n");
	return false;
}

void np_scene_renderer_finish(void)
{
	if (!renderer.device) return;
	vkDeviceWaitIdle(renderer.device);
	if (renderer.descriptor_pool)
		vkDestroyDescriptorPool(renderer.device, renderer.descriptor_pool, NULL);
	if (renderer.sampler)
		vkDestroySampler(renderer.device, renderer.sampler, NULL);
	if (renderer.pipeline)
		vkDestroyPipeline(renderer.device, renderer.pipeline, NULL);
	if (renderer.pipeline_layout)
		vkDestroyPipelineLayout(renderer.device, renderer.pipeline_layout, NULL);
	if (renderer.descriptor_layout)
		vkDestroyDescriptorSetLayout(renderer.device, renderer.descriptor_layout, NULL);
	if (renderer.load_render_pass)
		vkDestroyRenderPass(renderer.device, renderer.load_render_pass, NULL);
	if (renderer.clear_render_pass)
		vkDestroyRenderPass(renderer.device, renderer.clear_render_pass, NULL);
	if (renderer.fence)
		vkDestroyFence(renderer.device, renderer.fence, NULL);
	if (renderer.command_pool)
		vkDestroyCommandPool(renderer.device, renderer.command_pool, NULL);
	memset(&renderer, 0, sizeof(renderer));
}

bool np_scene_renderer_draw(struct np_vk_surface_buffer *output,
	                        uint32_t width, uint32_t height,
	                        const struct np_scene_layer *layers,
	                        uint32_t layer_count, bool clear)
{
	if (!renderer.device || !output || !output->view || !width || !height ||
	    !layers || !layer_count || layer_count > NP_SCENE_MAX_LAYERS)
		return false;
	if (clear && can_transfer_fast_path(layers, layer_count, width, height))
		return draw_transfer_fast_path(output, width, height, layers, layer_count);
	if (vkWaitForFences(renderer.device, 1, &renderer.fence,
	                    VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
	    vkResetFences(renderer.device, 1, &renderer.fence) != VK_SUCCESS ||
	    vkResetCommandBuffer(renderer.command, 0) != VK_SUCCESS ||
	    vkResetDescriptorPool(renderer.device, renderer.descriptor_pool, 0) != VK_SUCCESS)
		return false;

	VkDescriptorSetLayout layouts[NP_SCENE_MAX_LAYERS];
	VkDescriptorSet sets[NP_SCENE_MAX_LAYERS];
	for (uint32_t i = 0; i < layer_count; i++) layouts[i] = renderer.descriptor_layout;
	VkDescriptorSetAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = renderer.descriptor_pool,
		.descriptorSetCount = layer_count,
		.pSetLayouts = layouts,
	};
	if (vkAllocateDescriptorSets(renderer.device, &allocate, sets) != VK_SUCCESS)
		return false;

	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (vkBeginCommandBuffer(renderer.command, &begin) != VK_SUCCESS)
		return false;

	VkImageMemoryBarrier barriers[NP_SCENE_MAX_LAYERS + 1];
	uint32_t barrier_count = 0;
	for (uint32_t i = 0; i < layer_count; i++) {
		if (!layers[i].image || !layers[i].view || !layers[i].layout) continue;
		barriers[barrier_count++] = (VkImageMemoryBarrier) {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = *layers[i].layout,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = layers[i].image,
			.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			                      .levelCount = 1, .layerCount = 1 },
		};
		*layers[i].layout = VK_IMAGE_LAYOUT_GENERAL;
	}
	barriers[barrier_count++] = (VkImageMemoryBarrier) {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = output->host_dirty
			? VK_ACCESS_HOST_WRITE_BIT : VK_ACCESS_MEMORY_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = output->layout,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = output->image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		                      .levelCount = 1, .layerCount = 1 },
	};
	vkCmdPipelineBarrier(renderer.command,
	                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT |
	                     VK_PIPELINE_STAGE_HOST_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                     0, 0, NULL, 0, NULL, barrier_count, barriers);

	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkRenderPass active_render_pass = clear
		? renderer.clear_render_pass : renderer.load_render_pass;
	VkFramebufferCreateInfo framebuffer_info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = active_render_pass,
		.attachmentCount = 1,
		.pAttachments = &output->view,
		.width = output->width,
		.height = output->height,
		.layers = 1,
	};
	if (vkCreateFramebuffer(renderer.device, &framebuffer_info, NULL,
	                        &framebuffer) != VK_SUCCESS)
		return false;
	VkClearValue clear_value = { .color = { .float32 = { 0, 0, 0, 0 } } };
	VkRenderPassBeginInfo render = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = active_render_pass,
		.framebuffer = framebuffer,
		.renderArea = { .offset = {0, 0}, .extent = { width, height } },
		.clearValueCount = clear ? 1u : 0u,
		.pClearValues = clear ? &clear_value : NULL,
	};
	vkCmdBeginRenderPass(renderer.command, &render, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(renderer.command, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                  renderer.pipeline);
	VkViewport viewport = { 0, 0, (float)width, (float)height, 0, 1 };
	VkRect2D scissor = { .offset = {0, 0}, .extent = {width, height} };
	vkCmdSetViewport(renderer.command, 0, 1, &viewport);
	vkCmdSetScissor(renderer.command, 0, 1, &scissor);

	for (uint32_t i = 0; i < layer_count; i++) {
		const struct np_scene_layer *layer = &layers[i];
		VkDescriptorImageInfo image = {
			.sampler = renderer.sampler,
			.imageView = layer->view,
			.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		};
		VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = sets[i],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image,
		};
		vkUpdateDescriptorSets(renderer.device, 1, &write, 0, NULL);
		struct scene_push push = {
			.destination = { layer->destination_x0 * 2.0f / width - 1.0f,
			                 layer->destination_y0 * 2.0f / height - 1.0f,
			                 layer->destination_x1 * 2.0f / width - 1.0f,
			                 layer->destination_y1 * 2.0f / height - 1.0f },
			.source = { layer->source_u0, layer->source_v0,
			            layer->source_u1, layer->source_v1 },
			.opaque = layer->opaque ? 1u : 0u,
		};
		vkCmdBindDescriptorSets(renderer.command, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                        renderer.pipeline_layout, 0, 1, &sets[i], 0, NULL);
		vkCmdPushConstants(renderer.command, renderer.pipeline_layout,
		                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		                   0, sizeof(push), &push);
		vkCmdDraw(renderer.command, 6, 1, 0, 0);
	}
	vkCmdEndRenderPass(renderer.command);
	if (vkEndCommandBuffer(renderer.command) != VK_SUCCESS) {
		vkDestroyFramebuffer(renderer.device, framebuffer, NULL);
		return false;
	}
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &renderer.command,
	};
	bool ok = vkQueueSubmit(renderer.queue, 1, &submit, renderer.fence) == VK_SUCCESS &&
	          vkWaitForFences(renderer.device, 1, &renderer.fence,
	                          VK_TRUE, UINT64_MAX) == VK_SUCCESS;
	vkDestroyFramebuffer(renderer.device, framebuffer, NULL);
	if (ok) {
		output->layout = VK_IMAGE_LAYOUT_GENERAL;
		output->host_dirty = false;
	}
	return ok;
}
