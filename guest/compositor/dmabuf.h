#ifndef NP_DMABUF_H
#define NP_DMABUF_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct np_sync_point;

/// Advertises linux-dmabuf for the selected backend. RemotePipe imports
/// single-plane ARGB/XRGB images for encoding; VMPipe maps the exported
/// virtio-gpu resource directly to the host-visible GPU object.
void np_dmabuf_advertise(struct wl_display *display, int drm_fd);

/// A buffer created from linux-dmabuf, or NULL if this wl_buffer is wl_shm.
struct np_gpu_buffer {
	uint32_t resource_id;
	int32_t width;
	int32_t height;
	int32_t stride;
	/* Normalized WL_SHM_FORMAT_ARGB8888 or WL_SHM_FORMAT_XRGB8888. */
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
enum np_gpu_read_result {
	NP_GPU_READ_READY = 0,
	NP_GPU_READ_WAIT,
	NP_GPU_READ_FAILED,
};
/* Check the producer fence without taking a host-read reference.  This lets
 * the Wayland commit stay queued on a pollable fd before current state changes. */
enum np_gpu_read_result np_gpu_buffer_render_status(
	struct np_gpu_buffer *buffer, int *wait_fd);
/* The commit state machine has already waited for the producer before making
 * a buffer current. Scene construction only takes an ownership hold; repeating
 * VIRTGPU_WAIT/export-sync-file per layer would duplicate that synchronization. */
bool np_gpu_buffer_acquire_host_read(struct np_gpu_buffer *buffer);
void np_gpu_buffer_end_host_read(struct np_gpu_buffer *buffer);
/* RemotePipe returns tightly packed/readable pixels for the short encoder copy. */
bool np_gpu_buffer_begin_cpu_read(
	struct np_gpu_buffer *buffer, const unsigned char **pixels);
void np_gpu_buffer_end_cpu_read(struct np_gpu_buffer *buffer);
bool np_gpu_buffer_is_busy(struct np_gpu_buffer *buffer);
/* Signal an explicit-sync release point once this buffer has no current
 * surface owners and no host Metal reads. Takes ownership of point. */
void np_gpu_buffer_queue_release(
	struct np_gpu_buffer *buffer, struct np_sync_point *point);

#endif
