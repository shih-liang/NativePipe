#ifndef NATIVEPIPE_TEXT_INPUT_H
#define NATIVEPIPE_TEXT_INPUT_H

#include <stdint.h>

struct np_server;
struct np_surface;
struct wl_client;

void np_text_input_manager_bind(struct wl_client *client, void *data,
                                uint32_t version, uint32_t id);
void np_text_input_focus_changed(struct np_server *server,
                                 struct np_surface *previous,
                                 struct np_surface *next);
void np_text_input_deliver(struct np_server *server, const char *commit_text,
                           const char *preedit_text, int32_t cursor_begin,
                           int32_t cursor_end, int32_t delete_before,
                           int32_t delete_after);

#endif
