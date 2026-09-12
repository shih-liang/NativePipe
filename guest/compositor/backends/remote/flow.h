#ifndef NP_REMOTE_FLOW_H
#define NP_REMOTE_FLOW_H
#include <stdbool.h>
#include <stddef.h>

/* Receiver-paced application queue, not a second TCP congestion controller.
 * Keep roughly one measured roundtrip in SSH; reduce the budget when that
 * roundtrip grows, instead of leaving megabytes ahead of control replies. */
struct np_remote_flow {
    struct { size_t bytes; double sent; } pending[128];
    size_t bytes, window;
    unsigned head, count;
    double minimum_rtt, adjusted;
    unsigned slow_at_floor;
    bool waiting;
};
static inline size_t np_flow_allow(struct np_remote_flow *f, size_t remaining)
{
    if (!f->window) f->window = 65536;
    size_t chunk = f->window / 4;
    if (chunk < 1024) chunk = 1024;
    if (chunk > 16384) chunk = 16384;
    if (chunk > remaining) chunk = remaining;
    if (f->count == 128 || f->bytes + chunk > f->window) {
        f->waiting = true;
        return 0;
    }
    return chunk;
}
static inline void np_flow_sent(struct np_remote_flow *f, size_t bytes, double now)
{
    unsigned index = (f->head + f->count++) % 128;
    f->pending[index].bytes = bytes;
    f->pending[index].sent = now;
    f->bytes += bytes;
}
static inline bool np_flow_ack(struct np_remote_flow *f, size_t bytes, double now)
{
    if (!bytes || bytes > f->bytes) return false;
    f->bytes -= bytes;
    double rtt = 0;
    while (bytes) {
        size_t take = f->pending[f->head].bytes;
        if (take > bytes) take = bytes;
        bytes -= take;
        f->pending[f->head].bytes -= take;
        rtt = now - f->pending[f->head].sent;
        if (!f->pending[f->head].bytes) {
            f->head = (f->head + 1) % 128;
            f->count--;
        }
    }
    if (rtt <= 0) return true;
    if (!f->minimum_rtt || rtt < f->minimum_rtt) f->minimum_rtt = rtt;
    if (now - f->adjusted >= f->minimum_rtt) {
        double tolerance = 0.030;
        if (rtt - f->minimum_rtt > tolerance + 0.000001) {
            f->window = f->window * 3 / 4;
            if (f->window < 4096) f->window = 4096;
            /* Relearn a changed path only after sustained delay at the floor,
             * where our own queue can no longer explain a large RTT increase. */
            if (f->window == 4096 && ++f->slow_at_floor >= 8) {
                f->minimum_rtt = rtt; f->slow_at_floor = 0;
            }
        } else {
            f->slow_at_floor = 0;
            if (f->waiting) {
                f->window *= 2;
                if (f->window > 1048576) f->window = 1048576;
            }
        }
        f->waiting = false;
        f->adjusted = now;
    }
    return true;
}
#endif
