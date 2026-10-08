#ifndef NP_PRESENTATION_CLOCK_H
#define NP_PRESENTATION_CLOCK_H
#include <stdbool.h>
#include <stdint.h>

#define NP_PRESENTATION_CLOCK_VM_MAX_RTT_NS UINT64_C(50000000)
/* A remote reply must fit the existing pending request's 250 ms window. */
#define NP_PRESENTATION_CLOCK_REMOTE_MAX_RTT_NS UINT64_C(250000000)

/* A host sample lies between the guest send/receive times. The midpoint
 * estimate has at most half the measured RTT of transport uncertainty. Keep
 * low-RTT samples within an externally managed epoch. A clock discontinuity
 * requires a new epoch and a fresh clock; it must not rewrite saved scene
 * anchors. Never tag this software conversion as a hardware clock. */
struct np_presentation_clock {
    uint64_t host, guest, rtt, sampled_at;
    uint64_t epoch, epoch_host, maximum_rtt_ns; /* epoch_host: latest independently validated host instant. */
    bool valid;
};
static inline uint64_t np_presentation_clock_maximum_rtt(const struct np_presentation_clock *clock)
{
    return clock->maximum_rtt_ns ? clock->maximum_rtt_ns : NP_PRESENTATION_CLOCK_VM_MAX_RTT_NS;
}
static inline bool np_presentation_clock_discontinuous(
    const struct np_presentation_clock *clock,
    uint64_t sent, uint64_t received, uint64_t host)
{
    if (!clock->valid || !host || received < sent ||
        received - sent > np_presentation_clock_maximum_rtt(clock)) return false;
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
    if (!host || received < sent || received - sent > np_presentation_clock_maximum_rtt(clock)) return false;
    uint64_t rtt = received - sent, midpoint = sent + rtt / 2;
    if (clock->valid) {
        if (received < clock->sampled_at || host < clock->host || host < clock->epoch_host ||
            np_presentation_clock_discontinuous(clock, sent, received, host))
            return false;
        /* On SSH, a slower path must not replace a better offset simply
         * because two seconds passed. Its wider uncertainty permits a much
         * larger jump. Still validate continuity before keeping the anchor. */
        if (rtt > clock->rtt && (np_presentation_clock_maximum_rtt(clock) >
            NP_PRESENTATION_CLOCK_VM_MAX_RTT_NS || received - clock->sampled_at < 2000000000u))
            return true;
    }
    *clock = (struct np_presentation_clock){
        .host = host, .guest = midpoint, .rtt = rtt, .sampled_at = received,
        .epoch = clock->epoch, .epoch_host = clock->epoch_host,
        .maximum_rtt_ns = clock->maximum_rtt_ns, .valid = true,
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
