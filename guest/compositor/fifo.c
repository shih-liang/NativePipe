#include "fifo.h"

#include "compositor_internal.h"
#include "fifo-v1-server-protocol.h"

#include <stdlib.h>
#include <wayland-server-core.h>

static void fifo_resource_destroy(struct wl_resource *resource) {
	struct np_fifo *fifo = wl_resource_get_user_data(resource);
	if (!fifo) return;
	if (fifo->surface && fifo->surface->fifo == fifo)
		fifo->surface->fifo = NULL;
	free(fifo);
}

static void fifo_destroy_request(struct wl_client *client,
	                             struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static struct np_surface *fifo_surface_or_error(struct wl_resource *resource) {
	struct np_fifo *fifo = wl_resource_get_user_data(resource);
	if (fifo && fifo->surface) return fifo->surface;
	wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
	                       "the associated wl_surface was destroyed");
	return NULL;
}

static void fifo_set_barrier(struct wl_client *client,
	                         struct wl_resource *resource) {
	(void)client;
	struct np_surface *surface = fifo_surface_or_error(resource);
	if (surface) surface->pending_fifo_set_barrier = true;
}

static void fifo_wait_barrier(struct wl_client *client,
	                          struct wl_resource *resource) {
	(void)client;
	struct np_surface *surface = fifo_surface_or_error(resource);
	if (surface) surface->pending_fifo_wait_barrier = true;
}

static const struct wp_fifo_v1_interface fifo_implementation = {
	.set_barrier = fifo_set_barrier,
	.wait_barrier = fifo_wait_barrier,
	.destroy = fifo_destroy_request,
};

static void fifo_manager_destroy(struct wl_client *client,
	                             struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static void fifo_manager_get_fifo(struct wl_client *client,
	                              struct wl_resource *manager_resource,
	                              uint32_t id,
	                              struct wl_resource *surface_resource) {
	struct np_surface *surface = wl_resource_get_user_data(surface_resource);
	if (!surface) return;
	if (surface->fifo) {
		wl_resource_post_error(manager_resource,
		                       WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
		                       "a fifo object already exists for this surface");
		return;
	}

	struct np_fifo *fifo = calloc(1, sizeof(*fifo));
	if (!fifo) {
		wl_client_post_no_memory(client);
		return;
	}
	fifo->surface = surface;
	fifo->resource = wl_resource_create(client, &wp_fifo_v1_interface, 1, id);
	if (!fifo->resource) {
		free(fifo);
		wl_client_post_no_memory(client);
		return;
	}
	surface->fifo = fifo;
	wl_resource_set_implementation(fifo->resource, &fifo_implementation,
	                               fifo, fifo_resource_destroy);
}

static const struct wp_fifo_manager_v1_interface fifo_manager_implementation = {
	.destroy = fifo_manager_destroy,
	.get_fifo = fifo_manager_get_fifo,
};

void np_fifo_manager_bind(struct wl_client *client, void *data,
	                          uint32_t version, uint32_t id) {
	(void)data;
	struct wl_resource *resource = wl_resource_create(
		client, &wp_fifo_manager_v1_interface, version > 1 ? 1 : version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &fifo_manager_implementation,
	                               NULL, NULL);
}
