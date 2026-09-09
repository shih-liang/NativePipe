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
};
static inline struct np_remote_backend *np_remote_backend(const struct np_server *server)
{
    return server ? (struct np_remote_backend *)server->backend_state : NULL;
}
#endif
