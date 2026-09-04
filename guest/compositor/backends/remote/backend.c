#include "backend.h"
#include "backend_internal.h"
#include "compositor.h"
#include "compositor_internal.h"
#include "dmabuf.h"

#include <stdio.h>
#include <string.h>

/// Bare-metal remote compositor: TCP NPIP/NPEN + H.264 encode.
/// No VM or virtio-gpu objects are linked.
int np_backend_run(int argc, char **argv)
{
	struct np_remote_backend backend;
	memset(&backend, 0, sizeof(backend));
	backend.watched_host_fd = -1;
	backend.watched_media_fd = -1;
	return np_frontend_run(argc, argv, &backend);
}

bool np_backend_prepare(struct np_server *server)
{
	(void)server;
	fprintf(stderr,
	        "[wayland] remote backend: TCP 1025/1026 and immutable H.264 resources\n");
	return true;
}

void np_backend_advertise_globals(struct np_server *server)
{
	np_dmabuf_advertise(server->display, -1);
}

void np_backend_finish(struct np_server *server)
{
	(void)server;
}
