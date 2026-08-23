#ifndef NP_COMPOSITOR_INTERNAL_H
#define NP_COMPOSITOR_INTERNAL_H

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

struct np_box {
	int32_t x, y, width, height;
};

struct np_server {
	struct wl_display *display;
	struct wl_list surfaces;
	struct wl_list shm_textures;
	struct np_host host;
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
	uint32_t drag_focus_surface;
	struct wl_event_source *host_connection_source;
	struct wl_event_source *scene_retry_timer;
	int watched_host_fd;
	uint32_t watched_host_mask;
	char session_socket[128];
};

struct np_input {
	struct wl_list link;
	struct wl_resource *resource;
	struct np_server *server;
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

struct np_surface_update {
	struct wl_list link;
	struct np_surface *surface;
	struct wl_resource *buffer;
	struct wl_listener buffer_destroy;
	bool buffer_set;
	int scale;
	bool geometry_set;
	int32_t geometry_x, geometry_y, geometry_width, geometry_height;
	bool viewport_changed;
	struct np_viewport_state viewport;
	bool transform_changed;
	int32_t transform;
	bool input_region_changed;
	bool input_region_set;
	struct np_region_state input_region;
	bool opaque_region_changed;
	bool opaque_region_set;
	struct np_region_state opaque_region;
	struct np_box damage;
	bool set_fifo_barrier;
	struct np_sync_point *acquire_point;
	struct np_sync_point *release_point;
	uint32_t finishes_host_configure_serial;
	uint32_t presentation_id;
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
	struct wl_resource *toplevel;
	struct wl_resource *popup;
	struct wl_resource *decoration;
	struct wl_resource *fractional_scale;
	int reported_scale;
	struct wl_resource *viewport;
	struct np_viewport_state pending_viewport;
	struct np_viewport_state viewport_state;
	bool pending_viewport_changed;
	uint32_t window_id;
	uint32_t configure_serial;
	bool host_configure_in_flight;
	uint32_t host_configure_serial;
	bool host_configure_pending;
	int32_t host_configure_pending_width;
	int32_t host_configure_pending_height;
	uint32_t host_configure_pending_state_bits;
	uint32_t host_configure_acked_serial;
	bool host_configure_acked;
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

	struct np_fifo *fifo;
	bool pending_fifo_set_barrier;
	bool pending_fifo_wait_barrier;
	bool fifo_barrier_active;
	uint32_t fifo_barrier_presentation_id;

	struct wl_resource *subsurface;
	struct np_surface *parent;
	bool above_parent;
	int32_t sub_x, sub_y;
	bool pending_sub_position_set;
	int32_t pending_sub_x, pending_sub_y;
	bool host_sub_position_dirty;
	bool sync;
	struct wl_resource *cached_buffer;
	struct wl_listener cached_buffer_destroy;
	bool has_cached_buffer;
	uint32_t cached_presentation_id;
	bool cached_set_fifo_barrier;
	struct np_sync_point *cached_release_point;

	struct np_box pending;
	struct np_box owed[2];
	char *title;
	char *app_id;
	int32_t minimum_width, minimum_height;
	int32_t maximum_width, maximum_height;
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

#endif
