#ifndef NP_DMABUF_H
#define NP_DMABUF_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct np_sync_point;

/// Guest-side linux-dmabuf. Mesa Venus exports a virtio-gpu resource as a
/// dma-buf; its resource id maps directly to the existing host MTLTexture.
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

/* Queued commits retain the imported object independently of wl_resource. */
void np_gpu_buffer_retain(struct np_gpu_buffer *buffer);
void np_gpu_buffer_drop(struct np_gpu_buffer *buffer);

/// Current-surface and host-read ownership are independent of wl_resource
/// lifetime. A client may destroy the protocol object immediately after attach;
/// the imported resource remains alive until both counters reach zero.
void np_gpu_buffer_acquire_current(struct np_gpu_buffer *buffer);
void np_gpu_buffer_release_current(struct np_gpu_buffer *buffer);
bool np_gpu_buffer_begin_host_read(struct np_gpu_buffer *buffer);
void np_gpu_buffer_end_host_read(struct np_gpu_buffer *buffer);
bool np_gpu_buffer_is_busy(struct np_gpu_buffer *buffer);
/* Signal an explicit-sync release point once this buffer has no current
 * surface owners and no host Metal reads. Takes ownership of point. */
void np_gpu_buffer_queue_release(
	struct np_gpu_buffer *buffer, struct np_sync_point *point);

#endif
