#include "applications.h"
#include "compositor_internal.h"
#include "windowwire.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int ready(int fd, uint32_t mask, void *data)
{
    (void)fd; (void)mask;
    struct np_server *s = data;
    struct np_app_reply reply;
    /* Bound each dispatch so catalog loading cannot monopolize the loop. */
    for (int i = 0; i < 4 && np_apps_take(s->applications, &reply); ++i) {
        if (s->host_session_ready && (!reply.generation || reply.generation == s->application_generation))
            (void)np_backend_send_binary(s, reply.data, reply.length);
        free(reply.data);
    }
    np_backend_session_sync(s);
    return 0;
}
bool np_applications_init(struct np_server *s)
{
    s->applications = np_apps_start();
    if (!s->applications) return false;
    s->application_source = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
        np_apps_fd(s->applications), WL_EVENT_READABLE, ready, s);
    if (!s->application_source) { np_apps_stop(s->applications); s->applications = NULL; return false; }
    return true;
}
void np_applications_finish(struct np_server *s)
{
    if (s->application_source) wl_event_source_remove(s->application_source);
    np_apps_stop(s->applications);
}
bool np_applications_handle_command(const unsigned char *payload, size_t length, void *data)
{
    if (length < 8 || memcmp(payload, "NPW2", 4) || payload[5] != 27)
        return np_input_handle_host_binary(payload, length, data);
    struct np_server *s = data;
    struct np_window_reader r;
    if (!np_window_reader_init(&r, payload, length, NP_WINDOW_HOST_TO_GUEST)) return false;
    uint32_t token = np_window_read_u32(&r), action = np_window_read_u32(&r);
    char *id = np_window_read_string(&r);
    if (!id || !np_window_reader_finished(&r) || !s->host_session_ready) { free(id); return false; }
    bool ok = np_apps_request(s->applications, s->application_generation, token, action, id);
    free(id);
    if (!ok && errno == EAGAIN) {
        /* Admission failure belongs to this request, not the window session. */
        struct np_window_message m;
        np_window_message_init(&m, NP_WINDOW_GUEST_TO_HOST, 4);
        if (m.ok) memcpy(m.data, "NPAP", 4);
        np_window_put_u32(&m, token);
        np_window_put_string(&m, "The application service is busy. Try again.");
        ok = m.ok && np_backend_send_binary(s, m.data, m.len);
        np_window_message_clear(&m);
    }
    return ok;
}
