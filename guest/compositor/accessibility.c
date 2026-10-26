#include "accessibility.h"
#include "accessibility_worker.h"
#include "backend.h"
#include "compositor_internal.h"
#include "windowwire.h"
#include "xwayland.h"
#include <errno.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>

static int ready(int fd, uint32_t mask, void *data)
{
    (void)fd; (void)mask;
    struct np_server *s = data;
    struct np_a11y_reply reply;
    for (int i = 0; i < 4 && np_a11y_take(s->accessibility, &reply); ++i) {
        if (s->host_session_ready && reply.generation == s->application_generation)
            (void)np_backend_send_binary(s, reply.data, reply.length);
        free(reply.data);
    }
    np_backend_session_sync(s);
    return 0;
}

bool np_accessibility_init(struct np_server *s)
{
    s->accessibility = np_a11y_start();
    if (!s->accessibility) return false;
    s->accessibility_source = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
        np_a11y_fd(s->accessibility), WL_EVENT_READABLE, ready, s);
    if (!s->accessibility_source) { np_a11y_stop(s->accessibility); s->accessibility = NULL; return false; }
    return true;
}

void np_accessibility_finish(struct np_server *s)
{
    if (s->accessibility_source) wl_event_source_remove(s->accessibility_source);
    s->accessibility_source = NULL;
    np_a11y_stop(s->accessibility);
    s->accessibility = NULL;
}

static bool fail(struct np_server *s, uint32_t token, const char *message)
{
    unsigned char *data;
    size_t length;
    if (!np_a11y_encode_reply(token, message, "", &data, &length)) return false;
    bool ok = np_backend_send_binary(s, data, length);
    free(data);
    return ok;
}

/* NP_HOST_ACCESSIBILITY_REQUEST: token, window, operation, flags, number,
 * element, argument. Failures belong to the request, not the window session. */
bool np_accessibility_handle_command(struct np_server *s, const unsigned char *payload, size_t length)
{
    struct np_window_reader r;
    if (!np_window_reader_init(&r, payload, length, NP_WINDOW_HOST_TO_GUEST)) return false;
    struct np_a11y_request request = {
        .generation = s->application_generation,
        .token = np_window_read_u32(&r),
    };
    uint32_t window = np_window_read_u32(&r);
    request.operation = np_window_read_u32(&r);
    request.flags = np_window_read_u32(&r);
    request.number = np_window_read_f64(&r);
    char *element = np_window_read_string(&r), *argument = np_window_read_string(&r);
    if (!element || !argument || !np_window_reader_finished(&r) || !request.token || !s->host_session_ready) {
        free(element); free(argument);
        return false;
    }
    struct np_surface *surface = np_surface_by_window(s, window);
    const char *problem = NULL;
    if (!s->accessibility) problem = "Accessibility isn't available in this Linux session.";
    else if (!surface || !surface->toplevel || !surface->resource) problem = "The window closed. List windows again.";
    if (!problem) {
        struct wl_client *client = wl_resource_get_client(surface->resource);
        pid_t pid = 0;
        if (!np_xwayland_owns_client(s, client)) wl_client_get_credentials(client, &pid, NULL, NULL);
        request.pid = pid;
        request.geometry_x = surface->geometry_set ? surface->geometry_x : 0;
        request.geometry_y = surface->geometry_set ? surface->geometry_y : 0;
        request.title = g_strdup(surface->title ? surface->title : "");
        request.app_id = g_strdup(surface->app_id ? surface->app_id : "");
        request.element = g_strdup(element);
        request.argument = g_strdup(argument);
        if (!np_a11y_request(s->accessibility, &request))
            problem = errno == EAGAIN ? "The accessibility service is busy. Try again."
                                      : "The accessibility request is invalid.";
        g_free(request.title); g_free(request.app_id); g_free(request.element); g_free(request.argument);
    }
    free(element); free(argument);
    return !problem || fail(s, request.token, problem);
}
