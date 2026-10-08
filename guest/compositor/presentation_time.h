#ifndef NP_PRESENTATION_TIME_H
#define NP_PRESENTATION_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct np_server;
struct np_surface;
struct wl_client;
struct wl_display;
struct np_window_reader;
struct np_presentation_identity {
    uint64_t session, epoch, sent;
    uint32_t owner, presentation;
};
/* Both NPSN and contextual NPW2 commits use the same outcome identity. */
bool np_presentation_time_packet_identity(const void *packet, size_t size,
    struct np_presentation_identity *identity);

/* A capable backend advertises only after initial clock calibration. Submitted
 * feedback and its immutable clock anchor survive surface and connection loss.
 * FIFO latches and pixel-read release never complete these protocol objects. */
void np_presentation_time_init(struct np_server *server);
void np_presentation_time_enable(struct np_server *server, bool capable);
void np_presentation_time_advertise(struct np_server *server);
void np_presentation_time_reset(struct np_server *server);
void np_presentation_time_destroy(struct np_server *server);
uint64_t np_presentation_time_session(struct np_server *server);
uint64_t np_presentation_time_epoch(struct np_server *server);
bool np_presentation_time_emission_allowed(struct np_server *server);
bool np_presentation_time_prepare_scene(struct np_server *server,
    uint32_t owner, uint32_t scene, void *packet, size_t size);
/* Remote encoding completes asynchronously; refresh the send sample immediately
 * before its accepted scene is actually queued on the ordered display stream. */
void np_presentation_time_stamp_scene(struct np_server *server,
    uint32_t owner, uint32_t scene, void *packet, size_t size);
bool np_presentation_time_has_pending(struct np_surface *surface);
void np_presentation_time_commit(struct np_surface *surface, uint32_t commit);
void np_presentation_time_apply(struct np_surface *surface, uint32_t commit);
void np_presentation_time_scene(struct np_surface *surface, uint32_t commit, uint32_t scene);
void np_presentation_time_replay_surface(struct np_surface *surface, uint32_t owner, uint32_t scene);
void np_presentation_time_submitted(struct np_server *server, const void *packet, size_t size);
/* Definitely not admitted: remove only the preparation record, leaving the
 * surface's legitimate feedback attached to its commit for a later retry. */
void np_presentation_time_reject_scene(struct np_server *server, const void *packet, size_t size);
void np_presentation_time_discard_commit(struct np_surface *surface, uint32_t commit);
void np_presentation_time_discard_scene(struct np_server *server, uint32_t scene);
void np_presentation_time_discard_surface(struct np_surface *surface);
void np_presentation_time_clock_request(struct np_server *server);
void np_presentation_time_clock_sample(struct np_server *server, uint32_t token,
    uint64_t session, uint64_t epoch, uint64_t host);
void np_presentation_time_scene_clock_sample(struct np_server *server,
    uint64_t session, uint64_t epoch, uint32_t owner, uint32_t scene,
    uint64_t guest_sent, uint64_t host_received);
void np_presentation_time_result(struct np_server *server, uint64_t session,
    uint64_t epoch, uint32_t owner, uint32_t scene, uint64_t host,
    uint32_t refresh, uint32_t output);
void np_presentation_time_pause(struct np_server *server, uint64_t session, uint32_t token);
void np_presentation_time_drain(struct np_server *server, uint64_t session, uint32_t token);
void np_presentation_time_resume(struct np_server *server, uint64_t session, uint32_t token);
bool np_presentation_time_handle_command(struct np_server *server, struct np_window_reader *reader);
#endif
