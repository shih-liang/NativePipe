#ifndef NP_APPLICATIONS_H
#define NP_APPLICATIONS_H
#include "applications_worker.h"
struct np_server;
bool np_applications_init(struct np_server *server);
void np_applications_finish(struct np_server *server);
bool np_applications_handle_command(const unsigned char *payload, size_t length, void *data);
#endif
