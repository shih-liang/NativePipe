#ifndef NATIVEPIPE_FIFO_H
#define NATIVEPIPE_FIFO_H

#include <stdint.h>

struct wl_client;

void np_fifo_manager_bind(struct wl_client *client, void *data,
                          uint32_t version, uint32_t id);

#endif
