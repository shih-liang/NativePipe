/* Remote build stub: no Venus/linux-dmabuf import in MVP.
 * Provides symbols referenced by the shared compositor without linking dmabuf.c.
 */
#include "dmabuf.h"

struct np_gpu_buffer *np_gpu_buffer_get(struct wl_resource *buffer) {
	(void)buffer;
	return NULL;
}

void np_gpu_buffer_retain(struct np_gpu_buffer *buffer) { (void)buffer; }
void np_gpu_buffer_drop(struct np_gpu_buffer *buffer) { (void)buffer; }
void np_gpu_buffer_acquire_current(struct np_gpu_buffer *buffer) { (void)buffer; }
void np_gpu_buffer_release_current(struct np_gpu_buffer *buffer) { (void)buffer; }
enum np_gpu_read_result np_gpu_buffer_render_status(
	struct np_gpu_buffer *buffer, int *wait_fd)
{
	(void)buffer;
	if (wait_fd) *wait_fd = -1;
	return NP_GPU_READ_FAILED;
}
bool np_gpu_buffer_acquire_host_read(struct np_gpu_buffer *buffer)
{ (void)buffer; return false; }
void np_gpu_buffer_end_host_read(struct np_gpu_buffer *buffer) { (void)buffer; }
bool np_gpu_buffer_is_busy(struct np_gpu_buffer *buffer) { (void)buffer; return false; }
void np_gpu_buffer_queue_release(
	struct np_gpu_buffer *buffer, struct np_sync_point *point)
{ (void)buffer; (void)point; }

void np_dmabuf_advertise(struct wl_display *display, int drm_fd) {
	(void)display;
	(void)drm_fd;
}
