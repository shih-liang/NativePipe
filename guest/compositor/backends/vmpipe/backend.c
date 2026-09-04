#include "backend.h"
#include "backend_internal.h"
#include "compositor.h"
#include "compositor_internal.h"
#include "dmabuf.h"
#include "syncobj.h"
#include "virtio_resource.h"
#include "vk_context.h"
#include "vk_surface_buffer.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int np_backend_run(int argc, char **argv)
{
	struct np_vmpipe_backend backend;
	memset(&backend, 0, sizeof(backend));
	backend.drm_fd = -1;
	backend.watched_host_fd = -1;
	backend.watched_host_control_fd = -1;
	backend.watched_host_input_fd = -1;
	backend.watched_host_feedback_fd = -1;
	if (!np_vk_context_init(&backend.vk)) {
		fprintf(stderr,
		        "[wayland] failed to initialize compositor Venus device\n");
		return 1;
	}
	backend.vk_ready = true;
	np_vk_surface_buffer_set_device(
		backend.vk.physical_device, backend.vk.device);
	int result = np_frontend_run(argc, argv, &backend);
	np_vk_surface_buffer_clear_device();
	np_vk_context_destroy(&backend.vk);
	return result;
}

bool np_backend_prepare(struct np_server *server)
{
	struct np_vmpipe_backend *backend = np_vmpipe_backend(server);
	if (!backend || !backend->vk_ready) return false;
	backend->drm_fd = np_virtio_open_lookup_node();
	if (backend->drm_fd >= 0) return true;
	fprintf(stderr,
	        "[wayland] no virtio-gpu render node; cannot allocate host buffers\n");
	return false;
}

void np_backend_advertise_globals(struct np_server *server)
{
	struct np_vmpipe_backend *backend = np_vmpipe_backend(server);
	if (!backend) return;
	np_dmabuf_advertise(server->display, backend->drm_fd);
	np_syncobj_advertise(server->display, server);
}

void np_backend_finish(struct np_server *server)
{
	struct np_vmpipe_backend *backend = np_vmpipe_backend(server);
	if (!backend) return;
	if (backend->drm_fd >= 0) close(backend->drm_fd);
	backend->drm_fd = -1;
}

void np_backend_surface_destroy(struct np_surface *surface)
{
	(void)surface;
}
