/* Real output resource handlers and emitted Wayland events. Host geometry is
 * intentionally not mode/scale, so recomputing logical size would fail. */
#include "../scale.c"
#include "../subcompositor.c"
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned preferences, bounds;
static int32_t latest_width, latest_height;
enum { OUTPUT_ID = 2, OUTPUT_V1 = 4, OUTPUT_V2 = 6, OUTPUT_V3 = 8 };

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
	return surface && (surface->toplevel || surface->popup) ? surface : NULL;
}
bool np_surface_assign_role(struct np_surface *surface, enum np_surface_role role)
{
	if (surface->role != NP_SURFACE_ROLE_NONE && surface->role != role) return false;
	surface->role = role; return true;
}
bool np_trace_enabled(void) { return false; }
void np_surface_drop_queued_references(struct np_server *server, struct np_surface *surface) {}
void np_scene_note_structure_change(struct np_surface *surface) {}
bool np_surface_is_synchronized(struct np_surface *surface) { return surface->sync; }
void np_surface_apply_update(struct np_surface_update *update) {}
uint32_t np_presentation_next_id(struct np_server *server) { return ++server->next_presentation_id; }
void np_presentation_queue_scene(struct np_surface *surface, uint32_t id) {}
void np_surface_send_buffer_preferences(struct np_surface *surface, int scale)
{
	assert(surface->preferred_scale == scale); preferences++;
}
void np_xdg_output_bounds_changed(struct np_surface *surface)
{
	np_scale_surface_bounds(surface, &latest_width, &latest_height); bounds++;
}

struct event { uint32_t id; uint16_t opcode, size; const unsigned char *body; };
static unsigned char bytes[65536];
static struct event events[256];
static unsigned event_count;

static uint32_t integer(const void *data)
{
	uint32_t value; memcpy(&value, data, 4); return value;
}

static void collect(struct wl_client *client, int peer)
{
	wl_client_flush(client);
	size_t length = 0;
	for (;;) {
		ssize_t count = recv(peer, bytes + length, sizeof(bytes) - length, MSG_DONTWAIT);
		if (count < 0 && errno == EINTR) continue;
		if (count < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK); break; }
		assert(count > 0); length += (size_t)count; assert(length < sizeof(bytes));
	}
	event_count = 0;
	for (size_t offset = 0; offset < length;) {
		assert(length - offset >= 8 && event_count < 256);
		uint32_t header = integer(bytes + offset + 4);
		struct event *event = &events[event_count++];
		event->id = integer(bytes + offset); event->opcode = (uint16_t)header;
		event->size = (uint16_t)(header >> 16); event->body = bytes + offset + 8;
		assert(event->size >= 8 && event->size % 4 == 0 && event->size <= length - offset);
		offset += event->size;
	}
}

static unsigned count(uint32_t id, uint16_t opcode)
{
	unsigned count = 0;
	for (unsigned i = 0; i < event_count; i++)
		if (events[i].id == id && events[i].opcode == opcode) count++;
	return count;
}

static void geometry(uint32_t id, int32_t x, int32_t y, int32_t width, int32_t height)
{
	assert(count(id, 0) == 1 && count(id, 1) == 1);
	for (unsigned i = 0; i < event_count; i++) {
		struct event *event = &events[i]; if (event->id != id) continue;
		if (event->opcode == 0) {
			assert(event->size == 16);
			assert((int32_t)integer(event->body) == x && (int32_t)integer(event->body + 4) == y);
		}
		if (event->opcode == 1) {
			assert(event->size == 16);
			assert((int32_t)integer(event->body) == width && (int32_t)integer(event->body + 4) == height);
		}
	}
}

static void text(uint32_t id, uint16_t opcode, const char *expected)
{
	assert(count(id, opcode) == 1);
	for (unsigned i = 0; i < event_count; i++) {
		struct event *event = &events[i];
		if (event->id != id || event->opcode != opcode) continue;
		assert(integer(event->body) == strlen(expected) + 1);
		assert(!strcmp((const char *)event->body + 4, expected));
	}
}

static void scalar(uint32_t id, uint16_t opcode, uint32_t expected)
{
	assert(count(id, opcode) == 1);
	for (unsigned i = 0; i < event_count; i++) {
		struct event *event = &events[i];
		if (event->id == id && event->opcode == opcode) {
			assert(event->size == 12 && integer(event->body) == expected);
		}
	}
}

static void fixture_surface(struct np_server *server, struct np_surface *surface,
                            struct wl_client *client, uint32_t id)
{
	memset(surface, 0, sizeof(*surface));
	surface->server = server; surface->id = 100 + id;
	surface->preferred_scale = server->output_scale;
	wl_list_init(&surface->children); wl_list_init(&surface->sibling_link);
	wl_list_init(&surface->pending_stack_ops); wl_list_init(&surface->synchronized_updates);
	surface->resource = wl_resource_create(client, &wl_surface_interface, 6, id);
	assert(surface->resource);
	wl_resource_set_implementation(surface->resource, NULL, surface, NULL);
	wl_list_insert(server->surfaces.prev, &surface->link);
	np_scale_surface_enter_outputs(surface, client);
}

static void test_late_subsurface_inherits_parent_output(void)
{
	struct np_server server = {0}; server.display = wl_display_create(); assert(server.display);
	wl_list_init(&server.surfaces); wl_list_init(&server.output_states); wl_list_init(&server.outputs);
	server.output_scale = 1; server.output_width = 1920; server.output_height = 1080;
	np_scale_advertise(server.display, &server);
	struct np_host_output displays[] = {
		{.id = 77, .name = "First", .width = 1920, .height = 1080,
		 .pixel_width = 1920, .pixel_height = 1080, .scale = 1, .refresh_millihz = 60000},
		{.id = 88, .name = "Second", .x = 1920, .width = 1600, .height = 900,
		 .pixel_width = 3200, .pixel_height = 1800, .scale = 2, .refresh_millihz = 60000},
	};
	assert(np_scale_update_outputs(&server, displays, 2));
	int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
	struct wl_client *client = wl_client_create(server.display, pair[0]); assert(client);
	output_bind(client, output_state_by_id(&server, 77), 4, 2);
	output_bind(client, output_state_by_id(&server, 88), 4, 3);
	np_subcompositor_bind(client, &server, 1, 4);
	struct wl_resource *subcompositor = wl_client_get_object(client, 4); assert(subcompositor);
	struct np_surface parent, branch, nested;
	fixture_surface(&server, &parent, client, 5);
	parent.role = NP_SURFACE_ROLE_XDG_TOPLEVEL; parent.toplevel = parent.resource; parent.window_id = 1000;
	np_scale_window_output_changed(&server, parent.window_id, 88);
	assert(parent.output_id == 88 && parent.preferred_scale == 2);
	/* These new surfaces initially use the first output. The nested role is
	 * established before the branch is attached to the already mapped window. */
	fixture_surface(&server, &branch, client, 6);
	fixture_surface(&server, &nested, client, 7);
	subcompositor_get_subsurface(client, subcompositor, 8, nested.resource, branch.resource);
	assert(nested.subsurface && nested.parent == &branch);
	fractional_manager_bind(client, &server, 1, 9);
	struct wl_resource *fractional_manager = wl_client_get_object(client, 9); assert(fractional_manager);
	fractional_manager_get_scale(client, fractional_manager, 10, branch.resource);
	fractional_manager_get_scale(client, fractional_manager, 11, nested.resource);
	collect(client, pair[1]); scalar(10, 0, 120); scalar(11, 0, 120);
	/* No further host display or scale message is sent. The real attachment
	 * handler must repair both output associations and fractional preferences. */
	subcompositor_get_subsurface(client, subcompositor, 12, branch.resource, parent.resource);
	assert(branch.subsurface && branch.parent == &parent);
	assert(branch.output_id == 88 && nested.output_id == 88);
	assert(branch.preferred_scale == 2 && nested.preferred_scale == 2);
	assert(parent.output_id == 88 && parent.preferred_scale == 2);
	collect(client, pair[1]);
	for (uint32_t id = 6; id <= 7; id++) { scalar(id, 1, 2); scalar(id, 0, 3); }
	scalar(10, 0, 240); scalar(11, 0, 240);
	assert(count(5, 0) == 0 && count(5, 1) == 0);
	/* The same permanent role can be recreated on a different current output.
	 * Its existing descendants must follow that parent rather than stale state. */
	wl_resource_destroy(branch.subsurface);
	np_scale_window_output_changed(&server, parent.window_id, 77);
	assert(!branch.parent && branch.output_id == 88 && nested.output_id == 88);
	collect(client, pair[1]);
	subcompositor_get_subsurface(client, subcompositor, 13, branch.resource, parent.resource);
	assert(branch.output_id == 77 && nested.output_id == 77);
	assert(branch.preferred_scale == 1 && nested.preferred_scale == 1);
	collect(client, pair[1]);
	for (uint32_t id = 6; id <= 7; id++) { scalar(id, 1, 3); scalar(id, 0, 2); }
	scalar(10, 0, 120); scalar(11, 0, 120);
	wl_display_destroy_clients(server.display);
	wl_list_remove(&parent.link); wl_list_remove(&branch.link); wl_list_remove(&nested.link);
	struct np_output_state *state, *temporary;
	wl_list_for_each_safe(state, temporary, &server.output_states, link) output_state_remove(state);
	assert(wl_list_empty(&server.output_states));
	wl_display_destroy(server.display); close(pair[1]);
}

static void test_v3_with_output_v1_does_not_emit_unnegotiated_completion(void)
{
	struct np_server server = {0};
	server.display = wl_display_create(); assert(server.display);
	wl_list_init(&server.output_states); wl_list_init(&server.outputs); wl_list_init(&server.surfaces);
	server.output_scale = 1; server.output_width = 1024; server.output_height = 768;
	np_scale_advertise(server.display, &server);
	struct np_output_state *state = first_output_state(&server); assert(state);
	int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
	struct wl_client *client = wl_client_create(server.display, pair[0]); assert(client);
	output_bind(client, state, 1, 2);
	xdg_output_manager_bind(client, &server, 3, 3);
	get_xdg_output(client, wl_client_get_object(client, 3), 4, wl_client_get_object(client, 2));
	collect(client, pair[1]);
	geometry(4, state->x, state->y, state->width, state->height);
	assert(count(4, 2) == 0 && count(2, 2) == 0);
	struct np_host_output value = {
		.id = state->id, .name = "Display", .width = 1200, .height = 900,
		.pixel_width = 1200, .pixel_height = 900, .scale = 1, .refresh_millihz = 60000,
	};
	output_state_update(state, &value); collect(client, pair[1]);
	geometry(4, 0, 0, 1200, 900);
	assert(count(4, 2) == 0 && count(2, 2) == 0);
	wl_display_destroy_clients(server.display);
	output_state_remove(state);
	assert(wl_list_empty(&server.output_states) && wl_list_empty(&server.outputs));
	wl_display_destroy(server.display); close(pair[1]);
}

int main(void)
{
	struct np_server server = {0}; server.display = wl_display_create(); assert(server.display);
	wl_list_init(&server.output_states); wl_list_init(&server.outputs); wl_list_init(&server.surfaces);
	server.output_scale = 2; server.output_width = 3024; server.output_height = 1964;
	np_scale_advertise(server.display, &server);
	struct np_host_output value = {
		.id = 77, .name = "Built-in Display (Retina)", .x = -1536, .y = 40,
		.width = 1536, .height = 864, .pixel_width = 4096, .pixel_height = 2304,
		.scale = 2, .refresh_millihz = 60000,
	};
	assert(np_scale_update_outputs(&server, &value, 1));
	struct np_output_state *state = output_state_by_id(&server, 77); assert(state);
	int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
	struct wl_client *client = wl_client_create(server.display, pair[0]); assert(client);
	/* Direct handlers bypass the new_id decoder: keep client object IDs
	 * consecutive, rather than relying on skipped IDs having been reserved. */
	output_bind(client, state, 4, OUTPUT_ID);
	struct wl_resource *parent = wl_client_get_object(client, OUTPUT_ID); assert(parent);
	collect(client, pair[1]); text(OUTPUT_ID, 4, "NativePipe-77"); text(OUTPUT_ID, 5, value.name);
	for (uint32_t version = 1; version <= 3; version++) {
		xdg_output_manager_bind(client, &server, version, 2 * version + 1);
		get_xdg_output(client, wl_client_get_object(client, 2 * version + 1), 2 * version + 2, parent);
	}
	collect(client, pair[1]);
	for (uint32_t id = OUTPUT_V1; id <= OUTPUT_V3; id += 2) geometry(id, value.x, value.y, value.width, value.height);
	assert(count(OUTPUT_V1, 2) == 1 && count(OUTPUT_V2, 2) == 1 && count(OUTPUT_V3, 2) == 0 && count(OUTPUT_ID, 2) == 1);
	assert(count(OUTPUT_V1, 3) == 0 && count(OUTPUT_V1, 4) == 0);
	text(OUTPUT_V2, 3, "NativePipe-77"); text(OUTPUT_V3, 3, "NativePipe-77");
	text(OUTPUT_V2, 4, value.name); text(OUTPUT_V3, 4, value.name);
	assert(events[event_count - 1].id == OUTPUT_ID && events[event_count - 1].opcode == 2);
	/* Destroying the factories does not destroy the output extensions. */
	for (uint32_t id = 3; id <= 7; id += 2) wl_resource_destroy(wl_client_get_object(client, id));

	struct np_surface surface = {0}; surface.server = &server; surface.output_id = 77;
	surface.resource = wl_resource_create(client, &wl_surface_interface, 4, 9); assert(surface.resource);
	wl_resource_set_implementation(surface.resource, NULL, &surface, NULL);
	wl_list_insert(server.surfaces.prev, &surface.link);
	value.x = 0; value.width = 1600; value.height = 900; value.scale = 1;
	value.name = "Renamed display";
	assert(np_scale_update_outputs(&server, &value, 1));
	assert(surface.preferred_scale == 1 && preferences == 1 && bounds == 1);
	assert(latest_width == 1600 && latest_height == 900);
	collect(client, pair[1]);
	for (uint32_t id = OUTPUT_V1; id <= OUTPUT_V3; id += 2) geometry(id, value.x, value.y, value.width, value.height);
	assert(count(OUTPUT_V1, 2) == 1 && count(OUTPUT_V2, 2) == 1 && count(OUTPUT_V3, 2) == 0 && count(OUTPUT_ID, 2) == 1);
	assert(count(OUTPUT_ID, 4) == 0 && count(OUTPUT_V2, 3) == 0 && count(OUTPUT_V3, 3) == 0);
	assert(count(OUTPUT_V2, 4) == 0); text(OUTPUT_V3, 4, value.name);
	assert(events[event_count - 1].id == OUTPUT_ID && events[event_count - 1].opcode == 2);

	/* Each xdg resource retains the state independently of wl_output.release.
	 * Older objects can still complete updates with their own done event;
	 * v3 becomes inert when its completion resource no longer exists. */
	wl_resource_destroy(parent); collect(client, pair[1]);
	value.width = 1700; output_state_update(state, &value); collect(client, pair[1]);
	geometry(OUTPUT_V1, value.x, value.y, value.width, value.height);
	geometry(OUTPUT_V2, value.x, value.y, value.width, value.height);
	assert(count(OUTPUT_V1, 2) == 1 && count(OUTPUT_V2, 2) == 1 && count(OUTPUT_V3, 0) == 0);
	output_state_remove(state);
	assert(!state->global && output_state_has_resources(state));
	for (uint32_t id = OUTPUT_V1; id <= OUTPUT_V3; id += 2) wl_resource_destroy(wl_client_get_object(client, id));
	assert(wl_list_empty(&server.output_states) && wl_list_empty(&server.outputs));
	wl_display_destroy_clients(server.display); wl_display_destroy(server.display); close(pair[1]);
	test_v3_with_output_v1_does_not_emit_unnegotiated_completion();
	test_late_subsurface_inherits_parent_output();
	puts("xdg output logical geometry, versions and lifecycle passed");
	return 0;
}
