#include "cursor_shape.h"

#include "compositor_internal.h"
#include "cursor-shape-v1-server-protocol.h"
#include "window_events.h"
#include "windowwire.h"

#include <stdlib.h>
#include <wayland-server-core.h>

struct np_cursor_shape_device {
	struct wl_resource *resource;
	struct wl_resource *pointer;
	struct wl_listener pointer_destroy;
};

static void pointer_destroyed(struct wl_listener *listener, void *data) {
	struct np_cursor_shape_device *device = wl_container_of(
		listener, device, pointer_destroy);
	wl_list_remove(&device->pointer_destroy.link);
	wl_list_init(&device->pointer_destroy.link);
	device->pointer = NULL;
}

static void device_resource_destroy(struct wl_resource *resource) {
	struct np_cursor_shape_device *device = wl_resource_get_user_data(resource);
	if (!device) return;
	if (!wl_list_empty(&device->pointer_destroy.link))
		wl_list_remove(&device->pointer_destroy.link);
	free(device);
}

static void device_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void device_set_shape(struct wl_client *client, struct wl_resource *resource,
	                         uint32_t serial, uint32_t shape) {
	struct np_cursor_shape_device *device = wl_resource_get_user_data(resource);
	if (!wp_cursor_shape_device_v1_shape_is_valid(
		    shape, wl_resource_get_version(resource))) {
		wl_resource_post_error(resource,
		                       WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
		                       "invalid cursor shape %u", shape);
		return;
	}
	if (!device || !device->pointer) return;
	struct np_input *pointer = wl_resource_get_user_data(device->pointer);
	if (!pointer || serial != pointer->last_enter_serial) return;
	pointer->server->cursor_surface = NULL;
	uint32_t fields[] = {shape};
	np_window_event_send(pointer->server, NP_GUEST_CURSOR_SHAPE_CHANGED,
	                     fields, 1);
}

static const struct wp_cursor_shape_device_v1_interface device_implementation = {
	.destroy = device_destroy,
	.set_shape = device_set_shape,
};

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void create_device(struct wl_client *client, struct wl_resource *manager,
	                      uint32_t id, struct wl_resource *pointer) {
	struct wl_resource *resource = wl_resource_create(
		client, &wp_cursor_shape_device_v1_interface,
		wl_resource_get_version(manager), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_cursor_shape_device *device = calloc(1, sizeof(*device));
	if (!device) {
		wl_resource_destroy(resource);
		wl_client_post_no_memory(client);
		return;
	}
	device->resource = resource;
	device->pointer = pointer;
	wl_list_init(&device->pointer_destroy.link);
	if (pointer) {
		device->pointer_destroy.notify = pointer_destroyed;
		wl_resource_add_destroy_listener(pointer, &device->pointer_destroy);
	}
	wl_resource_set_implementation(
		resource, &device_implementation, device, device_resource_destroy);
}

static void manager_get_pointer(struct wl_client *client,
	                            struct wl_resource *manager, uint32_t id,
	                            struct wl_resource *pointer) {
	create_device(client, manager, id, pointer);
}

static void manager_get_tablet_tool(struct wl_client *client,
	                                struct wl_resource *manager, uint32_t id,
	                                struct wl_resource *tablet_tool) {
	/* NativePipe does not advertise tablet-v2, so this can only be reached by
	 * a client passing an object from an unsupported private protocol. Keep the
	 * resulting cursor-shape device inert as the specification requires when its
	 * input device is unavailable. */
	create_device(client, manager, id, NULL);
}

static const struct wp_cursor_shape_manager_v1_interface manager_implementation = {
	.destroy = manager_destroy,
	.get_pointer = manager_get_pointer,
	.get_tablet_tool_v2 = manager_get_tablet_tool,
};

static void manager_bind(struct wl_client *client, void *data,
	                     uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &wp_cursor_shape_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(
		resource, &manager_implementation, data, NULL);
}

void np_cursor_shape_advertise(struct wl_display *display,
	                           struct np_server *server) {
	wl_global_create(display, &wp_cursor_shape_manager_v1_interface, 1,
	                 server, manager_bind);
}
