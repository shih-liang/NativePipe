#include "decoration.h"
#include "compositor_internal.h"
#include "window_events.h"
#include "windowwire.h"
#include "xdg-decoration-server-protocol.h"

#include <wayland-server-core.h>

static void send_host_mode(struct np_surface *surface, bool server_side)
{
	if (!surface || !surface->toplevel)
		return;
	if (surface->decoration_negotiated &&
	    surface->decoration_server_side == server_side)
		return;

	surface->decoration_negotiated = true;
	surface->decoration_server_side = server_side;
	struct np_window_message message;
	np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST,
	                       NP_GUEST_DECORATION_MODE_CHANGED);
	np_window_put_u32(&message, surface->window_id);
	np_window_put_bool(&message, server_side);
	(void)np_window_event_send_message(surface->server, &message);
	np_window_message_clear(&message);
}

void np_decoration_use_client_default(struct np_surface *surface)
{
	/* NativePipe does not draw server decorations in the guest.  A client that
	 * does not opt into xdg-decoration therefore owns its CSD.  Toolkits which
	 * need native chrome (Qt among them) explicitly request SERVER_SIDE. */
	send_host_mode(surface, false);
}

static void configure(struct wl_resource *resource, uint32_t mode)
{
	struct np_surface *surface = wl_resource_get_user_data(resource);
	bool server_side = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	send_host_mode(surface, server_side);
	zxdg_toplevel_decoration_v1_send_configure(resource, mode);
}

static void decoration_destroy(struct wl_client *client,
	                           struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void decoration_set_mode(struct wl_client *client,
	                            struct wl_resource *resource, uint32_t mode)
{
	(void)client;
	if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
	    mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)
		return;
	configure(resource, mode);
}

static void decoration_unset_mode(struct wl_client *client,
	                              struct wl_resource *resource)
{
	(void)client;
	configure(resource, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

static const struct zxdg_toplevel_decoration_v1_interface decoration_implementation = {
	.destroy = decoration_destroy,
	.set_mode = decoration_set_mode,
	.unset_mode = decoration_unset_mode,
};

static void decoration_resource_destroy(struct wl_resource *resource)
{
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (surface && surface->decoration == resource)
		surface->decoration = NULL;
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void manager_get_decoration(struct wl_client *client,
	                               struct wl_resource *manager, uint32_t id,
	                               struct wl_resource *toplevel_resource)
{
	struct np_surface *surface = wl_resource_get_user_data(toplevel_resource);
	if (!surface || surface->decoration) {
		wl_resource_post_error(manager,
			ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
			"toplevel already has a decoration object");
		return;
	}

	struct wl_resource *decoration = wl_resource_create(
		client, &zxdg_toplevel_decoration_v1_interface,
		wl_resource_get_version(manager), id);
	if (!decoration) {
		wl_client_post_no_memory(client);
		return;
	}
	surface->decoration = decoration;
	wl_resource_set_implementation(decoration, &decoration_implementation,
	                               surface, decoration_resource_destroy);
	configure(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

static const struct zxdg_decoration_manager_v1_interface manager_implementation = {
	.destroy = manager_destroy,
	.get_toplevel_decoration = manager_get_decoration,
};

static void manager_bind(struct wl_client *client, void *data,
	                     uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *resource = wl_resource_create(
		client, &zxdg_decoration_manager_v1_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_implementation, NULL, NULL);
}

void np_decoration_advertise(struct wl_display *display, struct np_server *server)
{
	wl_global_create(display, &zxdg_decoration_manager_v1_interface, 1,
	                 server, manager_bind);
}
