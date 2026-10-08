/* Real activation handlers, Wayland resources and destroy listeners. Only the
 * scene-root lookup and native host transport are fixtures. */
#include "../activation.c"
#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned activations;
static uint32_t activated_window, activated_origin;
enum { SEAT_ID = 2, MANAGER_ID = 3, SURFACE_ID = 4 };
static struct { struct wl_client *client; uint32_t next_id; } clients[3];
static unsigned client_count;

struct np_surface *np_surface_by_window(struct np_server *server, uint32_t id)
{
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link)
		if ((surface->toplevel || surface->popup) && surface->window_id == id) return surface;
	return NULL;
}

struct np_surface *np_scene_root(struct np_surface *surface)
{
	while (surface && surface->role == NP_SURFACE_ROLE_SUBSURFACE) surface = surface->parent;
	return surface;
}

bool np_backend_send_binary(struct np_server *server, const void *payload, size_t length)
{
	struct np_window_reader reader;
	assert(np_window_reader_init(&reader, payload, length, NP_WINDOW_GUEST_TO_HOST));
	assert(reader.opcode == NP_GUEST_ACTIVATION_REQUESTED);
	activated_window = np_window_read_u32(&reader);
	activated_origin = np_window_read_u32(&reader);
	assert(np_window_read_u32(&reader) <= NP_ACTIVATION_TIMEOUT_MS);
	assert(np_window_reader_finished(&reader));
	activations++;
	return true;
}

static struct wl_client *client(struct np_server *server, int *peer)
{
	int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
	struct wl_client *client = wl_client_create(server->display, pair[0]);
	assert(client); *peer = pair[1];
	/* Direct handlers bypass request decoding, so fixture client IDs must be
	 * allocated consecutively after wl_display@1, without unreserved holes. */
	struct wl_resource *seat = wl_resource_create(client, &wl_seat_interface, 5, SEAT_ID);
	assert(seat); wl_resource_set_implementation(seat, NULL, server, NULL);
	activation_bind(client, server->activation, 1, MANAGER_ID);
	assert(wl_client_get_object(client, MANAGER_ID));
	assert(client_count < 3);
	clients[client_count].client = client;
	clients[client_count++].next_id = SURFACE_ID + 1;
	return client;
}

static void surface(struct np_server *server, struct np_surface *surface,
                    struct wl_client *client, uint32_t id, uint32_t window)
{
	memset(surface, 0, sizeof(*surface));
	surface->server = server; surface->id = id; surface->window_id = window;
	surface->role = NP_SURFACE_ROLE_XDG_TOPLEVEL; surface->mapped = true;
	surface->resource = wl_resource_create(client, &wl_surface_interface, 4, SURFACE_ID);
	assert(surface->resource);
	wl_resource_set_implementation(surface->resource, NULL, surface, NULL);
	/* Activation only observes whether an xdg_toplevel role is still alive. */
	surface->toplevel = surface->resource;
	wl_list_insert(server->surfaces.prev, &surface->link);
}

static struct wl_resource *token(struct wl_client *client, uint32_t serial,
                                struct wl_resource *origin, const char *app_id)
{
	uint32_t id = 0;
	for (unsigned i = 0; i < client_count; i++)
		if (clients[i].client == client) id = clients[i].next_id++;
	assert(id);
	get_activation_token(client, wl_client_get_object(client, MANAGER_ID), id);
	struct wl_resource *resource = wl_client_get_object(client, id);
	assert(resource);
	if (serial) token_set_serial(client, resource, serial, wl_client_get_object(client, SEAT_ID));
	if (origin) token_set_surface(client, resource, origin);
	if (app_id) token_set_app_id(client, resource, app_id);
	return resource;
}

static void mint(struct np_server *server, struct np_surface *origin,
                 uint32_t serial, const char *app_id, char value[33])
{
	struct wl_client *owner = wl_resource_get_client(origin->resource);
	np_activation_record_input(server, origin, serial);
	unsigned count = wl_list_length(&server->activation->grants);
	struct wl_resource *resource = token(owner, serial, origin->resource, app_id);
	token_commit(owner, resource);
	assert((unsigned)wl_list_length(&server->activation->grants) == count + 1);
	struct np_activation_grant *grant = wl_container_of(server->activation->grants.prev, grant, link);
	assert(strlen(grant->value) == 32); strcpy(value, grant->value);
	/* The exported bearer token must outlive its requesting proxy object. */
	wl_resource_destroy(resource);
}

static void use(struct wl_client *client, const char *value, struct np_surface *target)
{
	activate_request(client, wl_client_get_object(client, MANAGER_ID), value, target->resource);
}

int main(void)
{
	struct np_server server = {0}; server.display = wl_display_create();
	assert(server.display); wl_list_init(&server.surfaces);
	assert(np_activation_advertise(&server));
	int first_peer, second_peer, third_peer;
	struct wl_client *first = client(&server, &first_peer), *second = client(&server, &second_peer);
	struct wl_client *third = client(&server, &third_peer);
	struct np_surface origin, target, other;
	surface(&server, &origin, first, 10, 1);
	surface(&server, &target, second, 20, 2);
	surface(&server, &other, third, 30, 3);
	target.app_id = "org.example.Editor";
	server.focused_window = origin.window_id;
	struct wl_resource *request = token(first, 123, origin.resource, NULL);
	token_commit(first, request); assert(wl_list_empty(&server.activation->grants));

	/* A serial from another client, a destroyed seat, or background focus
	 * cannot turn a request into activation authority. */
	np_activation_record_input(&server, &origin, 124);
	request = token(second, 124, target.resource, NULL);
	token_set_serial(second, request, 124, wl_client_get_object(first, SEAT_ID));
	token_commit(second, request); assert(wl_list_empty(&server.activation->grants));
	request = token(first, 124, origin.resource, NULL);
	server.focused_window = target.window_id;
	token_commit(first, request); assert(wl_list_empty(&server.activation->grants));
	server.focused_window = origin.window_id;
	request = token(first, 124, origin.resource, NULL);
	server.activation->inputs[(server.activation->next_input - 1) % NP_ACTIVATION_INPUTS].time_ms =
		activation_now_ms() - NP_ACTIVATION_TIMEOUT_MS - 1;
	token_commit(first, request); assert(wl_list_empty(&server.activation->grants));

	char value[33]; mint(&server, &origin, 125, "org.example.Editor.desktop", value);
	use(second, value, &target);
	assert(activations == 1 && activated_window == 2 && activated_origin == 1);
	use(second, value, &target); assert(activations == 1);
	request = token(first, 125, origin.resource, NULL);
	token_commit(first, request); assert(wl_list_empty(&server.activation->grants));
	use(second, "unknown", &target); assert(activations == 1);

	/* Failed uses also consume tokens: changed focus, wrong app and unmapped
	 * targets cannot be retried after those conditions later become valid. */
	mint(&server, &origin, 126, "org.example.Other", value);
	use(second, value, &target); assert(wl_list_empty(&server.activation->grants));
	use(second, value, &target); assert(activations == 1);
	mint(&server, &origin, 127, NULL, value);
	server.focused_window = target.window_id; use(second, value, &target);
	server.focused_window = origin.window_id; use(second, value, &target); assert(activations == 1);
	mint(&server, &origin, 128, NULL, value);
	target.mapped = false; use(second, value, &target);
	target.mapped = true; use(second, value, &target); assert(activations == 1);
	mint(&server, &origin, 129, NULL, value);
	struct np_activation_grant *grant = wl_container_of(server.activation->grants.next, grant, link);
	grant->input_time_ms = activation_now_ms() - NP_ACTIVATION_TIMEOUT_MS - 1;
	use(second, value, &target); assert(activations == 1 && wl_list_empty(&server.activation->grants));

	mint(&server, &origin, 130, NULL, value);
	origin.mapped = false; np_activation_revoke_surface(&origin); origin.mapped = true;
	use(second, value, &target); assert(activations == 1 && wl_list_empty(&server.activation->grants));

	/* Grants are bounded; destroying a source releases even a full table and
	 * all paired listeners on that resource, without dangling client pointers. */
	for (unsigned i = 0; i < NP_ACTIVATION_TOKENS; i++) mint(&server, &origin, 200 + i, NULL, value);
	np_activation_record_input(&server, &origin, 300);
	request = token(first, 300, origin.resource, NULL);
	token_commit(first, request); assert(wl_list_length(&server.activation->grants) == NP_ACTIVATION_TOKENS);
	wl_resource_destroy(origin.resource); origin.resource = NULL; origin.mapped = false;
	assert(wl_list_empty(&server.activation->grants));
	for (unsigned i = 0; i < NP_ACTIVATION_INPUTS; i++) assert(!server.activation->inputs[i].surface);

	server.focused_window = other.window_id;
	np_activation_record_input(&server, &other, 400);
	request = token(third, 400, other.resource, NULL);
	wl_resource_destroy(wl_client_get_object(third, SEAT_ID));
	token_commit(third, request); assert(wl_list_empty(&server.activation->grants));
	request = token(third, 0, other.resource, NULL);
	wl_resource_destroy(other.resource); other.resource = NULL; other.mapped = false;
	token_commit(third, request); assert(wl_list_empty(&server.activation->grants));
	server.focused_window = 0;
	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display); assert(!server.activation);
	close(first_peer); close(second_peer); close(third_peer);
	puts("activation authority and lifecycle passed");
	return 0;
}
