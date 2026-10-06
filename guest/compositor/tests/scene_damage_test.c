/* Scene damage contract.
 *
 * The host repaints only what damage names. Its CAMetalLayer drawables rotate,
 * and a region nobody reports keeps whatever that drawable held several frames
 * earlier -- which for a widget that has just appeared means blank. So every
 * way the output can change has to reach the root's damage.
 *
 * These cases pin that down. Each "missed" case below is a regression that
 * showed up as GTK windows, under the GPU renderer, with blank widgets: the GPU
 * path creates and destroys offload subsurfaces and resizes their buffers,
 * which the cairo path never does, and none of those changes is buffer damage.
 *
 * Real scene.c, subcompositor.c, surface.c and region.c; only the backend,
 * scale and presentation collaborators are fixtures. */
#include "../scene.c"
#include "../subcompositor.c"
#include "../surface.c"
#include "../region.c"
#include <assert.h>
#include <sys/socket.h>

static int republished;
static struct np_surface *republished_root;
static uint32_t next_presentation = 1;

bool np_backend_describe_scene_source(
	const struct np_surface *surface, struct np_backend_scene_source *source) { return false; }
void np_backend_discard_scenes(struct np_surface *owner) {}
enum np_backend_hold_result np_backend_hold_scene(
	struct np_surface *owner, uint32_t presentation_id,
	struct np_surface *const *surfaces, size_t surface_count) { return NP_BACKEND_HOLD_READY; }
void np_backend_release_scene(struct np_surface *owner, uint32_t presentation_id) {}
bool np_backend_surface_has_current(const struct np_surface *surface) { return false; }
bool np_scale_resolve(const struct np_surface *surface, uint32_t buffer_width,
                      uint32_t buffer_height, struct np_surface_mapping *mapping) { return false; }
uint32_t np_presentation_next_id(struct np_server *server) { return next_presentation++; }
void np_presentation_queue_scene(struct np_surface *surface, uint32_t presentation_id) {
	republished++;
	republished_root = np_scene_root(surface);
}
void np_surface_apply_update(struct np_surface_update *update) {}
void np_surface_drop_queued_references(struct np_server *server, struct np_surface *surface) {}
bool np_surface_is_synchronized(struct np_surface *surface) { return false; }
bool np_trace_enabled(void) { return false; }
void np_surface_commit(struct wl_client *client, struct wl_resource *resource) {}
void np_surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t callback) {}
void np_presentation_clear_scene_wait(struct np_surface *surface) {}
void np_xdg_clear_configures(struct np_surface *surface) {}
void np_surface_update_destroy(struct np_surface_update *update, bool release_buffer) {}
void np_presentation_set_current_buffer(struct np_surface *surface, struct wl_resource *buffer,
                                       struct np_gpu_buffer *gpu, struct np_sync_point *release_point) {}
void np_syncobj_surface_destroyed(struct np_surface *surface) {}
void np_backend_surface_destroy(struct np_surface *surface) {}
void np_scale_surface_enter_outputs(struct np_surface *surface, struct wl_client *client) {}
bool np_window_event_send(struct np_server *server, uint8_t opcode,
                          const uint32_t *values, size_t count) { return true; }

/* Stands in for the xdg_toplevel resource. Only its presence is ever checked
 * on these paths; nothing dereferences it. */
static char toplevel_role_object;

static struct np_server server;
static struct np_surface root, child, grandchild;

static void init_surface(struct np_surface *surface, uint32_t id)
{
	memset(surface, 0, sizeof(*surface));
	surface->id = id;
	surface->server = &server;
	wl_list_init(&surface->children);
	wl_list_init(&surface->sibling_link);
	wl_list_init(&surface->pending_stack_ops);
	wl_list_init(&surface->pending_frame_callbacks);
	wl_list_init(&surface->blocked_updates);
	wl_list_init(&surface->synchronized_updates);
	wl_list_insert(server.surfaces.prev, &surface->link);
}

static void attach_child(struct np_surface *parent, struct np_surface *surface)
{
	surface->parent = parent;
	wl_list_insert(parent->children.prev, &surface->sibling_link);
}

/* toplevel root <- subsurface child <- subsurface grandchild, damage clean. */
static void reset_tree(void)
{
	memset(&server, 0, sizeof(server));
	wl_list_init(&server.surfaces);
	init_surface(&root, 1);
	init_surface(&child, 2);
	init_surface(&grandchild, 3);
	root.toplevel = (struct wl_resource *)&toplevel_role_object;
	attach_child(&root, &child);
	attach_child(&child, &grandchild);
	republished = 0;
	republished_root = NULL;
}

static bool damage_is_empty(const struct np_box *box)
{
	return box->width == 0 && box->height == 0;
}

static void init_update(struct np_surface_update *update)
{
	memset(update, 0, sizeof(*update));
	wl_list_init(&update->subsurface_positions);
	wl_list_init(&update->stack_ops);
	update->buffer_commit = NP_BUFFER_UNCHANGED;
}

/* ------------------------------------------------------------------------ */

static void test_buffer_damage_accumulates_per_surface_and_clears_tree_wide(void)
{
	reset_tree();
	struct np_box a = {0, 0, 10, 10}, b = {20, 20, 5, 5};
	np_scene_note_damage(&grandchild, &a, false);
	np_scene_note_damage(&grandchild, &b, false);
	assert(grandchild.scene_damage.x == 0 && grandchild.scene_damage.y == 0);
	assert(grandchild.scene_damage.width == 25 && grandchild.scene_damage.height == 25);
	/* Buffer damage alone is not a structural change. */
	assert(!root.scene_full_damage);

	/* Sending the root's scene retires damage everywhere beneath it, not only
	 * on the root -- otherwise a child's damage would be repainted forever. */
	np_scene_damage_sent(&root);
	assert(damage_is_empty(&grandchild.scene_damage));
	assert(!root.scene_full_damage);
}

static void test_damage_on_an_orphan_reaches_nothing(void)
{
	reset_tree();
	child.parent = NULL;   /* a subsurface whose parent chain is not rooted */
	grandchild.parent = &child;
	struct np_box a = {0, 0, 10, 10};
	np_scene_note_damage(&grandchild, &a, true);
	assert(damage_is_empty(&grandchild.scene_damage));
	assert(!root.scene_full_damage);
}

/* The trap np_scene_note_structure_change() exists to avoid. Once a surface
 * is unlinked it can no longer find its root, so noting damage *after* the
 * detach silently records nothing. Anyone tempted to "simplify" the detach
 * path by noting damage afterwards will fail here first. */
static void test_noting_damage_after_unlinking_is_too_late(void)
{
	reset_tree();
	wl_list_remove(&child.sibling_link);
	wl_list_init(&child.sibling_link);
	child.parent = NULL;
	np_scene_note_damage(&child, NULL, true);
	assert(!root.scene_full_damage);
}

static void test_structure_change_marks_the_root_and_tolerates_null(void)
{
	reset_tree();
	np_scene_note_structure_change(NULL);
	np_scene_note_structure_change(&root);
	assert(root.scene_full_damage);
	np_scene_damage_sent(&root);
	assert(!root.scene_full_damage);
}

/* Missed case 1: destroying a wl_subsurface. The area it covered has to be
 * repainted, but no commit reports that. */
static void test_detaching_a_subsurface_damages_the_former_root(void)
{
	reset_tree();
	np_subsurface_detach(&child);
	assert(child.parent == NULL);
	assert(wl_list_empty(&root.children));
	assert(root.scene_full_damage);
}

static void test_detaching_a_nested_subsurface_damages_the_top_root(void)
{
	reset_tree();
	np_subsurface_detach(&grandchild);
	/* The root, not the intermediate parent: damage is accounted per scene. */
	assert(root.scene_full_damage);
	assert(!child.scene_full_damage);
}

static void test_detaching_an_unparented_surface_changes_nothing(void)
{
	reset_tree();
	np_subsurface_detach(&root);
	assert(!root.scene_full_damage);
}

/* Missed case 1, through the real role object and its real destructor. It
 * must both damage the root and republish it, because no parent commit may
 * ever follow -- the client is free to stop committing the parent. */
static void test_subsurface_destructor_republishes_a_damaged_root(void)
{
	reset_tree();
	struct wl_display *display = wl_display_create();
	assert(display);
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	struct wl_client *client = wl_client_create(display, sockets[0]);
	assert(client);

	child.subsurface = wl_resource_create(client, &wl_subsurface_interface, 1, 0);
	assert(child.subsurface);
	wl_resource_set_implementation(child.subsurface, &subsurface_implementation,
	                               &child, subsurface_resource_destroy);
	wl_resource_destroy(child.subsurface);

	assert(child.subsurface == NULL);
	assert(child.parent == NULL);
	assert(wl_list_empty(&root.children));
	assert(republished == 1);
	assert(republished_root == &root);
	assert(root.scene_full_damage);

	wl_client_destroy(client);
	close(sockets[1]);
	wl_display_destroy(display);
}

/* Destroying wl_surface before its still-live role object exercises the
 * surface destructor itself. It must repaint immediately and detach both its
 * children and the role's user data before releasing the surface allocation. */
static void test_surface_destructor_republishes_with_a_live_subsurface_role(void)
{
	reset_tree();
	np_subsurface_detach(&grandchild);
	np_subsurface_detach(&child);
	root.scene_full_damage = false;
	struct wl_display *display = wl_display_create();
	assert(display);
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	struct wl_client *client = wl_client_create(display, sockets[0]);
	assert(client);
	struct np_surface *victim = calloc(1, sizeof(*victim));
	assert(victim);
	init_surface(victim, 4);
	attach_child(&root, victim);
	attach_child(victim, &grandchild);
	victim->resource = wl_resource_create(client, &wl_surface_interface, 6, 0);
	assert(victim->resource);
	wl_resource_set_implementation(victim->resource, &surface_implementation,
	                               victim, surface_resource_destroy);
	struct wl_resource *role = wl_resource_create(client, &wl_subsurface_interface, 1, 0);
	assert(role);
	victim->subsurface = role;
	wl_resource_set_implementation(role, &subsurface_implementation,
	                               victim, subsurface_resource_destroy);
	wl_resource_destroy(victim->resource);

	assert(wl_list_empty(&root.children));
	assert(grandchild.parent == NULL);
	assert(wl_resource_get_user_data(role) == NULL);
	assert(republished == 1 && republished_root == &root);
	assert(root.scene_full_damage);
	/* The later role destructor must neither read freed memory nor publish
	 * another scene using the former surface pointer. */
	wl_resource_destroy(role);
	assert(republished == 1);
	wl_client_destroy(client);
	close(sockets[1]);
	wl_display_destroy(display);
}

/* ------------------------------------------------------------------------ */
/* np_scene_update_changes_structure: the single place the commit path asks
 * whether an update moved anything buffer damage cannot describe. */

static void test_unchanged_update_is_not_structural(void)
{
	struct np_surface_update update;
	init_update(&update);
	assert(!np_scene_update_changes_structure(&update, 100, 50, 100, 50, false));
}

/* Missed case 3: a subsurface attaching a buffer of a different size. Its
 * buffer damage is in the new buffer's coordinates and cannot describe the
 * area the old size covered or the area the new size now covers. */
static void test_content_size_change_is_structural(void)
{
	struct np_surface_update update;
	init_update(&update);
	update.buffer_commit = NP_BUFFER_ATTACH;
	assert(np_scene_update_changes_structure(&update, 100, 50, 120, 50, false));
	assert(np_scene_update_changes_structure(&update, 100, 50, 100, 60, false));
	assert(np_scene_update_changes_structure(&update, 100, 50, 60, 30, false));
	/* Same size, new buffer: ordinary buffer damage describes it fully. */
	assert(!np_scene_update_changes_structure(&update, 100, 50, 100, 50, false));
}

static void test_first_attach_is_structural(void)
{
	struct np_surface_update update;
	init_update(&update);
	update.buffer_commit = NP_BUFFER_ATTACH;
	assert(np_scene_update_changes_structure(&update, 0, 0, 64, 64, false));
}

static void test_each_structural_flag_is_honoured(void)
{
	struct np_surface_update update;
#define EXPECT_STRUCTURAL(statement) do { \
		init_update(&update); statement; \
		assert(np_scene_update_changes_structure(&update, 10, 10, 10, 10, false)); \
	} while (0)
	EXPECT_STRUCTURAL(update.buffer_commit = NP_BUFFER_DETACH);
	EXPECT_STRUCTURAL(update.geometry_set = true);
	EXPECT_STRUCTURAL(update.popup_geometry_changed = true);
	EXPECT_STRUCTURAL(update.viewport_changed = true);
	EXPECT_STRUCTURAL(update.transform_changed = true);
	EXPECT_STRUCTURAL(update.offset_changed = true);
	EXPECT_STRUCTURAL(update.subsurface_state_changed = true);
#undef EXPECT_STRUCTURAL

	init_update(&update);
	assert(np_scene_update_changes_structure(&update, 10, 10, 10, 10, true));

	/* Queued position and restack operations are structural by presence. */
	struct wl_list entry;
	init_update(&update);
	wl_list_insert(&update.subsurface_positions, &entry);
	assert(np_scene_update_changes_structure(&update, 10, 10, 10, 10, false));
	init_update(&update);
	wl_list_insert(&update.stack_ops, &entry);
	assert(np_scene_update_changes_structure(&update, 10, 10, 10, 10, false));
}

int main(void)
{
	test_buffer_damage_accumulates_per_surface_and_clears_tree_wide();
	test_damage_on_an_orphan_reaches_nothing();
	test_noting_damage_after_unlinking_is_too_late();
	test_structure_change_marks_the_root_and_tolerates_null();
	test_detaching_a_subsurface_damages_the_former_root();
	test_detaching_a_nested_subsurface_damages_the_top_root();
	test_detaching_an_unparented_surface_changes_nothing();
	test_subsurface_destructor_republishes_a_damaged_root();
	test_surface_destructor_republishes_with_a_live_subsurface_role();
	test_unchanged_update_is_not_structural();
	test_content_size_change_is_structural();
	test_first_attach_is_structural();
	test_each_structural_flag_is_honoured();
	puts("scene damage contract: ok");
	return 0;
}
