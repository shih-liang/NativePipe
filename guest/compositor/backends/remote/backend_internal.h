#ifndef NP_REMOTE_BACKEND_INTERNAL_H
#define NP_REMOTE_BACKEND_INTERNAL_H
#include "compositor_internal.h"
#include "hostlink.h"
#include "medialink.h"
#include <sys/types.h>
struct np_remote_backend {
    struct np_host host;
    struct np_media media;
    uint32_t next_media_resource_id;
    struct wl_event_source *host_connection_source;
    int output_fd;
    char **command;
    pid_t application_pid;
    int application_fd;
    int exit_status;
    bool host_h264_hardware;
    /* At most two independent windows encode concurrently. Sources are reserved
     * until their immutable scene completes, preserving each AV1 reference chain. */
    struct np_remote_scene_job *jobs[2];
    uint32_t previous_owner;
    unsigned char *input_record;
    size_t input_size, input_offset;
    struct wl_event_source *refresh_timer;
    uint64_t refresh_deadline;
    uint64_t captured_frames, coalesced_frames, encoded_frames, sent_scenes;
    uint64_t displayed_scenes, discarded_scenes;
};
void np_remote_cancel_scenes(struct np_server *server, uint32_t surface_id);
void np_remote_finish_scenes(struct np_server *server);
bool np_remote_scene_available(const struct np_surface *surface);
void np_remote_scene_completed(struct np_server *server, uint32_t owner, uint32_t presentation, bool displayed, uint32_t interval_ns);
void np_remote_flush_encoded(struct np_server *server);
bool np_remote_submit_scene(struct np_server *server, const void *bytes, size_t size);
static inline struct np_remote_backend *np_remote_backend(const struct np_server *server)
{
    return server ? (struct np_remote_backend *)server->backend_state : NULL;
}
#endif
