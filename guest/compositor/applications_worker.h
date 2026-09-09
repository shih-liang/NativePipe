#ifndef NP_APPLICATIONS_WORKER_H
#define NP_APPLICATIONS_WORKER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Owned by the compositor, running with its uid and session environment.
 * No Wayland objects or backend calls cross into this worker. */
struct np_apps;
enum np_app_action { NP_APP_LIST, NP_APP_LAUNCH, NP_APP_ICON, NP_APP_APPEARANCE };
struct np_app_reply {
    uint64_t generation;
    unsigned char *data;
    size_t length;
};
struct np_apps *np_apps_start(void);
int np_apps_fd(struct np_apps *apps);
void np_apps_set_generation(struct np_apps *apps, uint64_t generation);
/* A rejected request sets EINVAL for invalid input or EAGAIN for a full queue. */
bool np_apps_request(struct np_apps *apps, uint64_t generation, uint32_t token,
                     uint32_t action, const char *id);
bool np_apps_take(struct np_apps *apps, struct np_app_reply *reply);
void np_apps_stop(struct np_apps *apps);
#endif
