/* Remote build stub: no Venus/linux-dmabuf import in MVP.
 * Provides symbols referenced by the shared compositor without linking dmabuf.c.
 */
#include "dmabuf.h"

#include <stddef.h>

struct np_gpu_buffer *np_gpu_buffer_get(struct wl_resource *buffer) {
	(void)buffer;
	return NULL;
}

void np_dmabuf_advertise(struct wl_display *display, int drm_fd) {
	(void)display;
	(void)drm_fd;
}
