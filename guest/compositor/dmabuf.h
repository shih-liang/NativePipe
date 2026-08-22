#ifndef NP_DMABUF_H
#define NP_DMABUF_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <vulkan/vulkan.h>

/// Guest-side linux-dmabuf. Mesa Venus exports a virtio-gpu blob as a
/// dma-buf. The compositor imports it as a source image, then composites it
/// into the window's ordinary BGRA scene VkImage.
void np_dmabuf_advertise(struct wl_display *display, int drm_fd);

/// A buffer created from linux-dmabuf, or NULL if this wl_buffer is wl_shm.
struct np_gpu_buffer {
	uint32_t resource_id;
	int32_t width;
	int32_t height;
	int32_t stride;
	uint32_t format;
};

struct np_gpu_buffer *np_gpu_buffer_get(struct wl_resource *buffer);

struct np_gpu_scene_image {
	VkImage image;
	VkImageView view;
	VkImageLayout *layout;
};

/// Wait for the implicit dma-buf producer fence, then expose the imported
/// client image directly to the window scene renderer. No snapshot is made.
bool np_gpu_buffer_prepare_scene(struct np_gpu_buffer *buffer,
                                struct np_gpu_scene_image *image);

struct np_vk_surface_buffer;

/// Wait for the client's implicit dma-buf fence and copy the image into the
/// selected window output. The output, not the wl_buffer, is what the host
/// receives and keeps displayed.
bool np_gpu_buffer_copy_to_output(struct np_gpu_buffer *buffer,
                                  struct np_vk_surface_buffer *output);
bool np_gpu_buffer_copy_to_output_region(
    struct np_gpu_buffer *buffer, struct np_vk_surface_buffer *output,
    int32_t source_x0, int32_t source_y0,
    int32_t source_x1, int32_t source_y1,
    int32_t destination_x0, int32_t destination_y0,
    int32_t destination_x1, int32_t destination_y1,
    bool clear);

#endif
