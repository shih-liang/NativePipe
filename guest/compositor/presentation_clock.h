#ifndef NP_PRESENTATION_CLOCK_H
#define NP_PRESENTATION_CLOCK_H
#include <stdbool.h>
#include <stdint.h>

/* A host sample lies between the guest send/receive times. The midpoint
 * estimate has at most half the measured RTT of transport uncertainty. Keep
 * low-RTT samples within an externally managed epoch. A clock discontinuity
 * requires a new epoch and a fresh clock; it must not rewrite saved scene
 * anchors. Never tag this software conversion as a hardware clock. */
struct np_presentation_clock {
    uint64_t host, guest, rtt, sampled_at;
    uint64_t epoch, epoch_host;
    bool valid;
};
static inline bool np_presentation_clock_discontinuous(
    const struct np_presentation_clock *clock,
    uint64_t sent, uint64_t received, uint64_t host)
{
    if (!clock->valid || !host || received < sent ||
        received - sent > 50000000u) return false;
    uint64_t rtt = received - sent, midpoint = sent + rtt / 2;
    __int128 offset = (__int128)midpoint - host;
    __int128 previous = (__int128)clock->guest - clock->host;
    __int128 difference = offset - previous;
    if (difference < 0) difference = -difference;
    return difference > (__int128)(clock->rtt / 2 + rtt / 2 + 1000000u);
}
static inline bool np_presentation_clock_sample_at(struct np_presentation_clock *clock,
    uint64_t sent, uint64_t received, uint64_t host)
{
    if (!host || received < sent || received - sent > 50000000u) return false;
    uint64_t rtt = received - sent, midpoint = sent + rtt / 2;
    if (clock->valid) {
        if (received < clock->sampled_at || host < clock->host ||
            np_presentation_clock_discontinuous(clock, sent, received, host))
            return false;
        if (received - clock->sampled_at < 2000000000u && rtt > clock->rtt)
            return true;
    }
    *clock = (struct np_presentation_clock){
        .host = host, .guest = midpoint, .rtt = rtt, .sampled_at = received,
        .epoch = clock->epoch, .epoch_host = clock->epoch_host, .valid = true,
    };
    return true;
}
static inline bool np_presentation_clock_convert(const struct np_presentation_clock *clock,
    uint64_t host, uint64_t now, uint64_t *guest)
{
    /* An anchor measures its own instant, not a historical offset. In
     * particular a post-pause anchor must never rewrite pre-pause display. */
    if (!clock->valid || !clock->host || !host || host < clock->host) return false;
    __int128 value = (__int128)host + clock->guest - clock->host;
    if (value <= 0 || value > UINT64_MAX || value > (__int128)now + clock->rtt / 2)
        return false;
    *guest = (uint64_t)value;
    return true;
}
#endif
