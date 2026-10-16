#ifndef NP_REMOTE_DROPS_H
#define NP_REMOTE_DROPS_H
#include "medialink.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

/* Mac files dropped on this computer's windows, served in place by a FUSE
 * mount whose NPFR v2 session rides the SSH stream as NPFS records. Started
 * only when the host announced support (NATIVEPIPE_HOST_DROPS=1). */
struct np_remote_drops {
    struct np_drop_service *service;
    struct np_media *media;
    int fd;
    pthread_t pump;
    bool running;
};
bool np_remote_drops_start(struct np_remote_drops *drops, struct np_media *media);
/* Host bytes for the session, in order. */
bool np_remote_drops_receive(struct np_remote_drops *drops, const unsigned char *bytes, size_t length);
void np_remote_drops_stop(struct np_remote_drops *drops);
#endif
