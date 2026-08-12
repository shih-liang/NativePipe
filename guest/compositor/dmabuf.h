#ifndef NP_DMABUF_H
#define NP_DMABUF_H

#include <stdint.h>
#include <wayland-server-core.h>

/// Guest-side linux-dmabuf. Mesa Venus exports a virtio-gpu blob as a
/// dmabuf; we turn the fd back into the resource id the host already
/// has. No pixels move. The host presents that resource on a CAMetalLayer.
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

#endif
