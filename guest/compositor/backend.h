#ifndef NP_BACKEND_H
#define NP_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct np_server;
struct np_surface;
struct np_gpu_buffer;
struct np_sync_point;
struct np_box;
struct np_window_frame;
struct wl_resource;
struct wl_event_loop;

enum np_buffer_commit_kind {
	NP_BUFFER_UNCHANGED,
	NP_BUFFER_ATTACH,
	NP_BUFFER_DETACH,
};

/* The executable entry is deliberately link-selected.  Both concrete
 * backends export this exact symbol; a binary links one and only one of them. */
int np_backend_run(int argc, char **argv);

/* Process/server ownership. */
bool np_backend_prepare(struct np_server *server);
void np_backend_advertise_globals(struct np_server *server);
void np_backend_finish(struct np_server *server);

/* Host session lifecycle and event-loop integration. */
void np_backend_session_reset_readiness(void);
bool np_backend_session_listen(struct np_server *server);
bool np_backend_session_set_socket(
	struct np_server *server, const char *socket);
void np_backend_session_attach(
	struct np_server *server, struct wl_event_loop *loop);
void np_backend_session_sync(struct np_server *server);
void np_backend_session_finish(struct np_server *server);

/* The frontend sends one ordered binary protocol.  Socket family, lane
 * topology, admission and reconnect handling are backend-private. */
bool np_backend_connected(const struct np_server *server);
bool np_backend_send_binary(
	struct np_server *server, const void *payload, size_t length);

/* Backend source description used by the shared scene serializer. */
struct np_backend_scene_source {
	uint32_t resource_id;
	uint32_t stride;
	uint16_t format;
	uint16_t flags;
};

bool np_backend_surface_has_current(const struct np_surface *surface);
/* Admission is backend-specific: VM resources and remote network credits
 * have different lifetimes. A false result leaves the latest scene dirty. */
bool np_backend_scene_ready(const struct np_surface *surface);
void np_backend_host_presented(struct np_server *server, uint32_t surface, uint32_t presentation);
bool np_backend_describe_scene_source(
	const struct np_surface *surface, struct np_backend_scene_source *source);

enum np_backend_hold_result {
	NP_BACKEND_HOLD_READY = 0,
	NP_BACKEND_HOLD_INVALID,
	NP_BACKEND_HOLD_NO_MEMORY,
};

/* A presentation hold is separate from wl_buffer.release.  The VM backend
 * retains resources until host release feedback; the encoded backend has no
 * live source object after the encoder copy and therefore implements a no-op. */
enum np_backend_hold_result np_backend_hold_scene(
	struct np_surface *owner, uint32_t presentation_id,
	struct np_surface *const *surfaces, size_t surface_count);
void np_backend_release_scene(
	struct np_surface *owner, uint32_t presentation_id);
void np_backend_discard_scenes(struct np_surface *owner);

/* Backend-specific metadata and per-surface resources. */
void np_backend_configure_frame(
	const struct np_surface *surface, struct np_window_frame *frame);
void np_backend_surface_destroy(struct np_surface *surface);

/* Import/copy/encode the source selected by one Wayland commit.  Concrete
 * backends preserve their own wl_buffer and explicit-sync release timing. */
bool np_backend_refresh_current_shm(
	struct np_surface *surface, uint32_t presentation_id,
	const struct np_box *damage);
void np_backend_publish_buffer(
	struct np_surface *surface, struct wl_resource *buffer,
	struct np_gpu_buffer *gpu_buffer,
	enum np_buffer_commit_kind buffer_commit, uint32_t presentation_id,
	struct np_sync_point *release_point, const struct np_box *damage);

#endif
