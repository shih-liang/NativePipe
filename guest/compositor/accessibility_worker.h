#ifndef NP_ACCESSIBILITY_WORKER_H
#define NP_ACCESSIBILITY_WORKER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AT-SPI2 client for agents, owned by the compositor and running with its uid
 * and session bus. Every D-Bus call blocks only this worker: an application
 * answering accessibility requests may itself be waiting on the compositor.
 * No Wayland objects cross into the worker; the main thread copies the target
 * window's identity into each request. */
struct np_a11y;
enum np_a11y_operation {
    NP_A11Y_TREE = 1,
    NP_A11Y_ACTION = 2,
    NP_A11Y_FOCUS = 3,
    NP_A11Y_SET_TEXT = 4,
    NP_A11Y_SET_VALUE = 5,
};
enum np_a11y_flags {
    NP_A11Y_INCLUDE_HIDDEN = 1u << 0,
    NP_A11Y_FULL_TEXT = 1u << 1,
};
#define NP_A11Y_MAX_ELEMENT 1024u
#define NP_A11Y_MAX_ARGUMENT 65536u
#define NP_A11Y_MAX_REPLY (1024u * 1024u)

struct np_a11y_request {
    uint64_t generation;
    uint32_t token, operation, flags;
    /* Tree depth, action index, or new value, depending on the operation. */
    double number;
    /* The window's client process, or 0 when unknown (Xwayland). */
    int32_t pid;
    /* xdg window geometry origin in surface coordinates. */
    int32_t geometry_x, geometry_y;
    char *title, *app_id, *element, *argument;
};
struct np_a11y_reply {
    uint64_t generation;
    unsigned char *data;
    size_t length;
};
struct np_a11y *np_a11y_start(void);
/* Bus addresses for tests; NULL uses the session bus's org.a11y.Bus. */
struct np_a11y *np_a11y_start_with_address(const char *address);
int np_a11y_fd(struct np_a11y *a11y);
/* Takes ownership of the request's strings. EINVAL for invalid input,
 * EAGAIN for a full queue. */
bool np_a11y_request(struct np_a11y *a11y, struct np_a11y_request *request);
bool np_a11y_take(struct np_a11y *a11y, struct np_a11y_reply *reply);
void np_a11y_stop(struct np_a11y *a11y);
/* NPAP reply 7: token, error message (empty on success), JSON document. */
bool np_a11y_encode_reply(uint32_t token, const char *error, const char *json,
                          unsigned char **data, size_t *length);
#endif
