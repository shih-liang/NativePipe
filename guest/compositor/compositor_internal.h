#ifndef NP_COMPOSITOR_INTERNAL_H
#define NP_COMPOSITOR_INTERNAL_H

#include "damage.h"
#include "hostlink.h"
#include "region.h"
#ifdef NP_REMOTE
#include "medialink.h"
#include "../encoder/encoder.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct np_gpu_buffer;
struct np_shm_texture;
struct np_sync_surface;
struct np_sync_point;
struct np_xwayland;

enum np_surface_role {
	NP_SURFACE_ROLE_NONE = 0,
	NP_SURFACE_ROLE_XDG_TOPLEVEL,
	NP_SURFACE_ROLE_XDG_POPUP,
	NP_SURFACE_ROLE_SUBSURFACE,
	NP_SURFACE_ROLE_CURSOR,
	NP_SURFACE_ROLE_DRAG_ICON,
	NP_SURFACE_ROLE_XWAYLAND,
};

struct np_server {
	struct wl_display *display;
	struct wl_list surfaces;
	struct wl_list shm_textures;
	/* Guest-to-host events remain on `host`. Host-to-guest paths use separate
	 * sockets so vsock credit and a slow writer cannot couple unrelated Wayland
	 * lifetimes. Remote TCP keeps its existing single control stream. */
	struct np_host host;
	#ifndef NP_REMOTE
	struct np_host host_control;
	struct np_host host_input;
	struct np_host host_feedback;
	#endif
	bool host_session_ready;
#ifdef NP_REMOTE
	struct np_media media;
#endif
	int drm_fd;
	uint32_t next_id;
	uint32_t next_presentation_id;
	int output_scale;
	int output_width;
	int output_height;
	struct wl_list outputs;
	struct wl_list pointers;
	struct wl_list keyboards;
	int keymap_fd;
	struct wl_resource *selection_source;
	struct wl_list data_devices;
	struct wl_list data_offers;
	char *host_mime[24];
	int host_mime_count;
	uint32_t next_clip_token;
	struct wl_list clip_reads;
	struct wl_list clip_writes;
	struct wl_list text_inputs;
	struct wl_resource *drag_source;
	struct wl_resource *drag_origin;
	struct wl_resource *drag_icon;
	uint32_t drag_focus_window;
	bool drag_dropped;
	double pointer_x, pointer_y;
	size_t keymap_size;
	uint32_t focused_window;
	uint32_t pointer_window;
	uint32_t pointer_surface;
	struct wl_client *pointer_grab_client;
	uint32_t pointer_grab_serial;
	uint32_t pointer_buttons;
	struct wl_client *last_input_client;
	uint32_t last_input_serial;
	struct wl_resource *cursor_surface;
	int32_t cursor_hotspot_x, cursor_hotspot_y;
	uint32_t drag_focus_surface;
	struct wl_event_source *host_connection_source;
	struct wl_event_source *scene_retry_timer;
	int watched_host_fd;
	uint32_t watched_host_mask;
	#ifndef NP_REMOTE
	struct wl_event_source *host_control_connection_source;
	struct wl_event_source *host_input_connection_source;
	struct wl_event_source *host_feedback_connection_source;
	int watched_host_control_fd;
	int watched_host_input_fd;
	int watched_host_feedback_fd;
	uint32_t watched_host_control_mask;
	uint32_t watched_host_input_mask;
	uint32_t watched_host_feedback_mask;
	#endif
	char session_socket[128];
	struct np_xwayland *xwayland;
	char xwayland_display[16];
	char xwayland_auth[256];
};

struct np_input {
	struct wl_list link;
	struct wl_resource *resource;
	struct np_server *server;
	uint32_t last_enter_serial;
};

struct np_output {
	struct wl_list link;
	struct wl_resource *resource;
};

struct np_viewport_state {
	bool source_set;
	wl_fixed_t source_x, source_y, source_width, source_height;
	bool destination_set;
	int32_t destination_width, destination_height;
};

struct np_frame_callback {
	struct wl_list link;
	struct wl_resource *resource;
	uint32_t presentation_id;
};

struct np_fifo {
	struct wl_resource *resource;
	struct np_surface *surface;
};

enum np_buffer_commit_kind {
	NP_BUFFER_UNCHANGED,
	NP_BUFFER_ATTACH,
	NP_BUFFER_DETACH,
};

struct np_surface_update {
	struct wl_list link;
	/* A commit captures the synchronized child CUs that existed at that exact
	 * commit boundary.  Later child commits remain in the child's queue. */
	struct wl_list dependencies;
	/* wl_subsurface position/stacking is double-buffered parent state, so it
	 * belongs to this parent CU rather than to the live child objects. */
	struct wl_list subsurface_positions;
	struct wl_list stack_ops;
	struct np_surface *surface;
	struct wl_resource *buffer;
	/* A wl_buffer resource may be destroyed while this commit waits. Preserve
	 * the submitted operation and imported object independently. */
	struct np_gpu_buffer *gpu_buffer;
	struct wl_listener buffer_destroy;
	enum np_buffer_commit_kind buffer_commit;
	int scale;
	bool geometry_set;
	int32_t geometry_x, geometry_y, geometry_width, geometry_height;
	bool popup_geometry_changed;
	int32_t popup_x, popup_y, popup_width, popup_height;
	bool viewport_changed;
	struct np_viewport_state viewport;
	bool transform_changed;
	int32_t transform;
	bool offset_changed;
	int32_t offset_x, offset_y;
	bool input_region_changed;
	bool input_region_set;
	struct np_region_state input_region;
	bool opaque_region_changed;
	bool opaque_region_set;
	struct np_region_state opaque_region;
	bool size_constraints_changed;
	int32_t minimum_width, minimum_height;
	int32_t maximum_width, maximum_height;
	struct np_box damage;
	bool set_fifo_barrier;
	bool wait_fifo_barrier;
	bool subsurface_state_changed;
	struct np_sync_point *acquire_point;
	struct np_sync_point *release_point;
	struct wl_event_source *wait_source;
	int wait_fd;
	uint32_t presentation_id;
};

struct np_subsurface_position_update {
	struct wl_list link;
	struct np_surface *child;
	int32_t x, y;
};

/* wl_subsurface stacking is double-buffered state of the parent. Keep the
 * requests in wire order so multiple restacks before one parent commit retain
 * their protocol-defined ordering. */
struct np_subsurface_stack_op {
	struct wl_list link;
	struct np_surface *child;
	struct np_surface *sibling;
	bool above;
};

/* xdg_surface.configure serials remain valid until ack_configure consumes the
 * named serial and every older one.  Keep the exact outstanding set instead of
 * comparing integers: serials are display-global and may wrap. */
struct np_xdg_configure {
	struct wl_list link;
	uint32_t serial;
	bool from_host;
	bool popup_geometry;
	int32_t popup_x, popup_y, popup_width, popup_height;
};

enum np_xdg_configure_phase {
	NP_XDG_NO_ROLE,
	NP_XDG_AWAITING_INITIAL_COMMIT,
	NP_XDG_AWAITING_INITIAL_ACK,
	NP_XDG_CONFIGURED,
};

struct np_surface {
	struct wl_list link;
	/* Active children are stored bottom-to-top. The below-parent group always
	 * precedes the above-parent group. */
	struct wl_list children;
	struct wl_list sibling_link;
	struct wl_list pending_stack_ops;
	struct np_server *server;
	struct wl_resource *resource;
	uint32_t id;
	struct wl_list pending_frame_callbacks;
	struct wl_list blocked_updates;
	struct wl_resource *pending_buffer;
	bool pending_buffer_set;
	/* Protocol state at the last wl_surface.commit boundary. This is separate
	 * from current_buffer, whose GPU lifetime may extend past a null commit. */
	bool committed_buffer_attached;
	/* Current Wayland source for a window scene. It remains busy while it is
	 * part of the scene and is replaced atomically by the next buffer commit. */
	struct wl_resource *current_buffer;
	struct wl_listener current_buffer_destroy;
	struct np_gpu_buffer *current_gpu;
	struct np_shm_texture *current_shm;
	struct np_sync_surface *syncobj;
	struct np_sync_point *current_release_point;
	int pending_scale;
	int scale;
	int32_t pending_transform;
	int32_t transform;
	bool pending_transform_changed;
	bool pending_offset_changed;
	int32_t pending_offset_x, pending_offset_y;
	int32_t buffer_offset_x, buffer_offset_y;
	bool pending_input_region_changed;
	bool pending_input_region_set;
	struct np_region_state pending_input_region;
	bool input_region_set;
	struct np_region_state input_region;
	bool pending_opaque_region_changed;
	bool pending_opaque_region_set;
	struct np_region_state pending_opaque_region;
	bool opaque_region_set;
	struct np_region_state opaque_region;

	struct wl_resource *xdg_surface;
	struct wl_resource *xdg_wm_base;
	enum np_surface_role role;
	struct wl_resource *toplevel;
	struct wl_resource *popup;
	struct wl_resource *decoration;
	uint32_t xwayland_window;
	bool xwayland_popup;
	struct wl_resource *fractional_scale;
	int preferred_scale;
	int reported_scale;
	struct wl_resource *viewport;
	struct np_viewport_state pending_viewport;
	struct np_viewport_state viewport_state;
	bool pending_viewport_changed;
	uint32_t window_id;
	struct wl_list xdg_configures;
	uint32_t latest_configure_serial;
	enum np_xdg_configure_phase xdg_configure_phase;
	bool mapped;
	bool host_configure_pending;
	int32_t host_configure_pending_width;
	int32_t host_configure_pending_height;
	uint32_t host_configure_pending_state_bits;
	bool host_configure_in_flight;
	uint32_t host_configure_acked_serial;
	bool host_configure_acked_from_host;
	bool host_configure_acked;
	/* The configure gate opens only when the commit that answered it reaches
	 * the output latch. Releasing it at wl_surface.commit lets FIFO-blocked
	 * resize commits create an unbounded chain of swapchains. */
	uint32_t host_configure_latch_presentation_id;
	/* Coalesce AppKit resize samples and keep at most one host configure in
	 * flight. A client rebuilding a Vulkan swapchain must never be flooded by
	 * every mouse sample it could not yet commit. */
	struct wl_event_source *host_configure_idle;
	bool pending_geometry_set;
	int32_t pending_geometry_x, pending_geometry_y;
	int32_t pending_geometry_width, pending_geometry_height;
	bool geometry_set;
	int32_t geometry_x, geometry_y;
	int32_t geometry_width, geometry_height;

	uint32_t pending_presentation_id;
	struct wl_list scene_presentations;
	bool scene_dirty;
	uint32_t scene_presentation_id;
	struct wl_event_source *scene_wait_source;
	int scene_wait_fd;
	/* Buffer-space damage accumulated until the next window scene crosses the
	 * host channel. Structural changes set scene_full_damage on the xdg root. */
	struct np_box scene_damage;
	bool scene_full_damage;

	struct np_fifo *fifo;
	bool pending_fifo_set_barrier;
	bool pending_fifo_wait_barrier;
	bool fifo_barrier_active;
	uint32_t fifo_barrier_presentation_id;

	struct wl_resource *subsurface;
	struct np_surface *parent;
	uint32_t popup_parent_window;
	int32_t popup_x, popup_y;
	int32_t popup_width, popup_height;
	int32_t popup_flip_x, popup_flip_y;
	uint32_t popup_constraint_adjustment;
	uint32_t popup_requested_token;
	bool popup_reactive;
	bool popup_geometry_acked;
	int32_t popup_acked_x, popup_acked_y;
	int32_t popup_acked_width, popup_acked_height;
	bool above_parent;
	int32_t sub_x, sub_y;
	bool pending_sub_position_set;
	int32_t pending_sub_x, pending_sub_y;
	bool host_sub_position_dirty;
	bool sync;
	/* Every wl_surface state change of a synchronized subsurface is latched by
	 * its parent commit, not just the attached buffer. */
	struct wl_list synchronized_updates;

	/* wl_surface.damage and damage_buffer use different coordinate spaces and
	 * remain separate until this pending state is captured by commit. */
	struct np_box pending_surface_damage;
	struct np_box pending_buffer_damage;
	char *title;
	char *app_id;
	int32_t minimum_width, minimum_height;
	int32_t maximum_width, maximum_height;
	int32_t pending_minimum_width, pending_minimum_height;
	int32_t pending_maximum_width, pending_maximum_height;
	bool pending_size_constraints_changed;
	bool has_grab;
	uint32_t focus_restore_window;
	bool decoration_negotiated;
	bool decoration_server_side;
	const char *last_format;
	int32_t last_width, last_height;
	uint32_t last_resource_id;
	uint32_t last_stride;
	const char *last_source;
	bool has_published;
	cJSON *pending_frame;
#ifdef NP_REMOTE
	struct np_encoder *encoder;
	uint16_t last_epoch;
#endif
};

bool np_surface_assign_role(struct np_surface *surface,
                            enum np_surface_role role);

/* Narrow services shared by protocol modules. */
bool np_trace_enabled(void);
struct np_surface *np_surface_by_window(struct np_server *server,
                                        uint32_t window_id);
struct np_surface *np_surface_by_id(struct np_server *server,
                                    uint32_t surface_id);
bool np_surface_is_toplevel(const struct np_surface *surface);
bool np_surface_is_popup(const struct np_surface *surface);

/* Core protocol registration. */
void np_compositor_bind(struct wl_client *client, void *data,
                        uint32_t version, uint32_t id);
void np_subcompositor_bind(struct wl_client *client, void *data,
                           uint32_t version, uint32_t id);
void np_seat_bind(struct wl_client *client, void *data,
                  uint32_t version, uint32_t id);
bool np_input_create_keymap(struct np_server *server);

/* Surface commit and subsurface state. */
void np_surface_commit(struct wl_client *client, struct wl_resource *resource);
void np_surface_apply_update(struct np_surface_update *update);
void np_surface_apply_unblocked(struct np_surface *surface);
void np_surface_update_destroy(struct np_surface_update *update,
                               bool release_buffer);
void np_surface_drop_queued_references(struct np_server *server,
                                       struct np_surface *surface);
bool np_surface_is_synchronized(struct np_surface *surface);
bool np_surface_watch_wait_fd(struct np_server *server, int fd,
                              struct wl_event_source **source,
                              int *stored_fd);
void np_surface_schedule_retry(struct np_server *server);
void np_subsurface_detach(struct np_surface *surface);
void np_subsurface_detach_tree(struct np_surface *surface);

/* Frame ownership, presentation feedback and host publication. */
void np_surface_frame(struct wl_client *client, struct wl_resource *resource,
                      uint32_t id);
uint32_t np_presentation_next_id(struct np_server *server);
bool np_presentation_bind_callbacks(struct np_surface *surface,
                                    uint32_t presentation_id);
bool np_presentation_has_unbound_callbacks(struct np_surface *surface);
void np_presentation_process_presented(struct np_server *server,
                                       uint32_t surface_id,
                                       uint32_t presentation_id);
void np_presentation_process_released(struct np_server *server,
                                      uint32_t surface_id,
                                      uint32_t presentation_id);
void np_presentation_finish_feedback(struct np_server *server);
void np_presentation_flush(struct np_server *server);
void np_presentation_clear_scene_wait(struct np_surface *surface);
void np_presentation_request_refresh(struct np_surface *surface,
                                     uint32_t presentation_id);
void np_presentation_add_viewport(struct np_surface *surface, cJSON *frame);
bool np_presentation_queue_last(struct np_surface *surface,
                                uint32_t presentation_id);
bool np_presentation_refresh_current_shm(struct np_surface *surface,
                                         uint32_t presentation_id,
                                         const struct np_box *damage);
void np_presentation_queue_scene(struct np_surface *surface,
                                 uint32_t presentation_id);
void np_presentation_publish_buffer(struct np_surface *surface,
                                    struct wl_resource *buffer,
                                    struct np_gpu_buffer *gpu_buffer,
                                    enum np_buffer_commit_kind buffer_commit,
                                    uint32_t presentation_id,
                                    struct np_sync_point *release_point,
                                    const struct np_box *damage);
void np_presentation_set_current_buffer(struct np_surface *surface,
                                        struct wl_resource *buffer,
                                        struct np_gpu_buffer *gpu_buffer,
                                        struct np_sync_point *release_point);

/* Host transport and command dispatch. */
void np_input_handle_host_binary(const unsigned char *payload, size_t length,
                                 void *user_data);
void np_input_handle_host_command(const char *name, cJSON *body,
                                  void *user_data);
void np_host_session_reset_readiness(void);
bool np_host_session_listen(struct np_server *server);
bool np_host_session_set_socket(struct np_server *server, const char *socket);
void np_host_session_attach(struct np_server *server,
                            struct wl_event_loop *loop);
void np_host_session_sync(struct np_server *server);
void np_host_session_pump(struct np_server *server);
void np_host_session_finish(struct np_server *server);

void np_set_keyboard_focus(struct np_server *server, uint32_t window_id);
void np_input_clear_pointer_focus_for_drag(struct np_server *server);
void np_input_restore_pointer_focus_after_drag(struct np_server *server,
                                               struct np_surface *surface);

#endif
