/* Host display times and guest CLOCK_MONOTONIC have unrelated origins.
 * Conversion must preserve the original display time, account for a paused VM,
 * and reject timestamps outside the measured transport uncertainty. */
#undef NDEBUG
#include "../presentation_clock.h"
#include <assert.h>
#include <stdio.h>

#define MS UINT64_C(1000000)
#define SECOND UINT64_C(1000000000)

static void expect_conversion(const struct np_presentation_clock *clock,
    uint64_t host, uint64_t now, uint64_t expected)
{
    uint64_t guest = UINT64_MAX;
    assert(np_presentation_clock_convert(clock, host, now, &guest));
    assert(guest == expected);
}

static void expect_rejection(const struct np_presentation_clock *clock,
    uint64_t host, uint64_t now)
{
    uint64_t guest = 123;
    assert(!np_presentation_clock_convert(clock, host, now, &guest));
    assert(guest == 123); /* A failed conversion cannot masquerade as now. */
}

static void expect_unchanged(const struct np_presentation_clock *clock,
    const struct np_presentation_clock *before)
{
    assert(clock->valid == before->valid && clock->host == before->host &&
           clock->guest == before->guest && clock->rtt == before->rtt &&
           clock->sampled_at == before->sampled_at && clock->epoch == before->epoch &&
           clock->epoch_host == before->epoch_host);
}

static void different_origins_preserve_display_time(void)
{
    struct np_presentation_clock clock = {0};
    /* Host origin is 8,000 seconds ahead of the guest. */
    assert(np_presentation_clock_sample_at(&clock,
        1000 * SECOND, 1000 * SECOND + 4 * MS, 9000 * SECOND + 2 * MS));
    expect_conversion(&clock, 9000 * SECOND + 12 * MS,
        1000 * SECOND + 80 * MS, 1000 * SECOND + 12 * MS);
    /* Receiving this feedback later does not turn its timestamp into receive time. */
    expect_conversion(&clock, 9000 * SECOND + 12 * MS,
        1000 * SECOND + 900 * MS, 1000 * SECOND + 12 * MS);

    /* The opposite sign of offset must work too. */
    clock = (struct np_presentation_clock){0};
    assert(np_presentation_clock_sample_at(&clock,
        9 * SECOND, 9 * SECOND + 4 * MS, 2 * SECOND + 2 * MS));
    expect_conversion(&clock, 2 * SECOND + 20 * MS,
        9 * SECOND + 100 * MS, 9 * SECOND + 20 * MS);
}

static void invalid_and_slow_samples_cannot_replace_calibration(void)
{
    struct np_presentation_clock clock = {0};
    expect_rejection(&clock, SECOND, SECOND);
    assert(np_presentation_clock_sample_at(&clock,
        SECOND, SECOND + 2 * MS, 10 * SECOND + MS));
    struct np_presentation_clock before = clock;
    assert(!np_presentation_clock_sample_at(&clock, 2 * SECOND, 2 * SECOND - 1, SECOND));
    expect_unchanged(&clock, &before);
    assert(!np_presentation_clock_sample_at(&clock, 2 * SECOND, 2 * SECOND + MS, 0));
    expect_unchanged(&clock, &before);
    assert(!np_presentation_clock_sample_at(&clock,
        2 * SECOND, 2 * SECOND + 50 * MS + 1, 11 * SECOND + 25 * MS));
    expect_unchanged(&clock, &before);
    assert(!np_presentation_clock_discontinuous(&clock,
        2 * SECOND, 2 * SECOND - 1, SECOND));
    assert(!np_presentation_clock_discontinuous(&clock,
        2 * SECOND, 2 * SECOND + MS, 0));
    assert(!np_presentation_clock_discontinuous(&clock,
        2 * SECOND, 2 * SECOND + 50 * MS + 1, 30 * SECOND));
    /* A delayed sample from before the saved anchor cannot move it backwards. */
    assert(!np_presentation_clock_sample_at(&clock,
        SECOND - 10 * MS, SECOND - 8 * MS, 10 * SECOND - 9 * MS));
    expect_unchanged(&clock, &before);
    assert(!np_presentation_clock_sample_at(&clock,
        SECOND, SECOND + 2 * MS, 10 * SECOND));
    expect_unchanged(&clock, &before);
    expect_rejection(&clock, 0, UINT64_MAX);

    /* Exactly 50 ms is within the bound; anything greater is rejected. */
    clock = (struct np_presentation_clock){0};
    assert(np_presentation_clock_sample_at(&clock,
        SECOND, SECOND + 50 * MS, 10 * SECOND + 25 * MS));
    assert(clock.rtt == 50 * MS && clock.guest == SECOND + 25 * MS);
}

static void fresh_calibration_keeps_the_lowest_rtt(void)
{
    struct np_presentation_clock clock = {0};
    assert(np_presentation_clock_sample_at(&clock,
        100 * SECOND, 100 * SECOND + 8 * MS, 900 * SECOND + 4 * MS));
    assert(np_presentation_clock_sample_at(&clock,
        100 * SECOND + 100 * MS, 100 * SECOND + 102 * MS,
        900 * SECOND + 101 * MS));
    assert(clock.rtt == 2 * MS);
    struct np_presentation_clock best = clock;
    assert(np_presentation_clock_sample_at(&clock,
        100 * SECOND + 200 * MS, 100 * SECOND + 212 * MS,
        900 * SECOND + 206 * MS));
    expect_unchanged(&clock, &best);
    expect_conversion(&clock, 900 * SECOND + 210 * MS,
        100 * SECOND + 220 * MS, 100 * SECOND + 210 * MS);

    /* An expired sample is refreshed even if the new transport is slower. */
    assert(np_presentation_clock_sample_at(&clock,
        103 * SECOND, 103 * SECOND + 12 * MS, 903 * SECOND + 6 * MS));
    assert(clock.rtt == 12 * MS && clock.sampled_at == 103 * SECOND + 12 * MS);
}

static void vm_pause_requires_an_explicit_new_epoch(void)
{
    struct np_presentation_clock clock = {.epoch = 7, .epoch_host = 50 * SECOND + MS};
    assert(np_presentation_clock_sample_at(&clock,
        20 * SECOND, 20 * SECOND + 2 * MS, 50 * SECOND + MS));
    struct np_presentation_clock before = clock;
    /* A valid sample with a ten-second offset jump cannot silently change the
     * epoch or reinterpret scenes that still own the old anchor. */
    assert(np_presentation_clock_discontinuous(&clock,
        20 * SECOND + 8 * MS, 20 * SECOND + 12 * MS, 60 * SECOND + 10 * MS));
    assert(!np_presentation_clock_sample_at(&clock,
        20 * SECOND + 8 * MS, 20 * SECOND + 12 * MS, 60 * SECOND + 10 * MS));
    expect_unchanged(&clock, &before);
    clock = (struct np_presentation_clock){
        .epoch = 8, .epoch_host = 60 * SECOND + 10 * MS,
    };
    assert(!np_presentation_clock_discontinuous(&clock,
        20 * SECOND + 8 * MS, 20 * SECOND + 12 * MS, 60 * SECOND + 10 * MS));
    assert(np_presentation_clock_sample_at(&clock,
        20 * SECOND + 8 * MS, 20 * SECOND + 12 * MS, 60 * SECOND + 10 * MS));
    assert(clock.rtt == 4 * MS && clock.host == 60 * SECOND + 10 * MS);
    assert(clock.epoch == 8 && clock.epoch_host == 60 * SECOND + 10 * MS);
    expect_conversion(&clock, 60 * SECOND + 20 * MS,
        20 * SECOND + 30 * MS, 20 * SECOND + 20 * MS);
}

static void discontinuity_excludes_measured_transport_uncertainty(void)
{
    struct np_presentation_clock clock = {.epoch = UINT64_MAX};
    assert(np_presentation_clock_sample_at(&clock,
        SECOND, SECOND + 2 * MS, 10 * SECOND + MS));
    /* Old half-RTT + new half-RTT + 1 ms is a tolerated offset difference. */
    assert(!np_presentation_clock_discontinuous(&clock,
        SECOND + 100 * MS, SECOND + 104 * MS, 10 * SECOND + 106 * MS));
    assert(np_presentation_clock_discontinuous(&clock,
        SECOND + 100 * MS, SECOND + 104 * MS, 10 * SECOND + 106 * MS + 1));
    assert(!np_presentation_clock_discontinuous(&clock,
        SECOND + 100 * MS, SECOND + 104 * MS, 10 * SECOND + 98 * MS));
    assert(np_presentation_clock_discontinuous(&clock,
        SECOND + 100 * MS, SECOND + 104 * MS, 10 * SECOND + 98 * MS - 1));
    assert(clock.epoch == UINT64_MAX && clock.epoch_host == 0);

    /* Both offset signs can be enormous; subtraction must not wrap uint64. */
    clock = (struct np_presentation_clock){0};
    assert(np_presentation_clock_sample_at(&clock,
        UINT64_MAX - 4 * MS, UINT64_MAX - 2 * MS, 100 * MS));
    assert(np_presentation_clock_discontinuous(&clock,
        UINT64_MAX - 2 * MS, UINT64_MAX, UINT64_MAX));
}

static void first_scene_keeps_its_actual_time_until_qualified(void)
{
    struct np_presentation_clock scene = {.epoch = 12};
    uint64_t host_display = 250 * SECOND + 18 * MS;
    /* The actual result may arrive before its clock reply; an unqualified
     * scene cannot use guest receive time as a substitute timestamp. */
    expect_rejection(&scene, host_display, 500 * SECOND + 25 * MS);
    assert(!np_presentation_clock_sample_at(&scene,
        500 * SECOND, 500 * SECOND + 50 * MS + 1, 250 * SECOND + 15 * MS));
    expect_rejection(&scene, host_display, 500 * SECOND + 60 * MS);
    assert(np_presentation_clock_sample_at(&scene,
        500 * SECOND, 500 * SECOND + 30 * MS, 250 * SECOND + 15 * MS));
    assert(scene.epoch == 12 && scene.epoch_host == 0);
    expect_conversion(&scene, host_display,
        500 * SECOND + 35 * MS, 500 * SECOND + 18 * MS);
    expect_conversion(&scene, host_display,
        500 * SECOND + 900 * MS, 500 * SECOND + 18 * MS);
    expect_rejection(&scene, 250 * SECOND + 14 * MS, 500 * SECOND + 900 * MS);
}

static void scene_sample_and_saved_anchor_are_independent_of_global_updates(void)
{
    struct np_presentation_clock global = {.epoch = 13};
    assert(np_presentation_clock_sample_at(&global,
        100 * SECOND, 100 * SECOND + 8 * MS, 900 * SECOND + 4 * MS));
    struct np_presentation_clock initial = global, scene = global;
    assert(np_presentation_clock_sample_at(&scene,
        100 * SECOND + 100 * MS, 100 * SECOND + 102 * MS,
        900 * SECOND + 101 * MS));
    assert(scene.rtt == 2 * MS && scene.epoch == global.epoch);
    expect_unchanged(&global, &initial);
    assert(np_presentation_clock_sample_at(&global,
        100 * SECOND + 200 * MS, 100 * SECOND + 202 * MS,
        900 * SECOND + 201 * MS));
    expect_rejection(&global, 900 * SECOND + 103 * MS, 100 * SECOND + 300 * MS);
    expect_conversion(&scene, 900 * SECOND + 103 * MS,
        100 * SECOND + 300 * MS, 100 * SECOND + 103 * MS);

    /* An independent historical scene sample can arrive after a newer global
     * anchor. Its own three timestamps qualify it without changing global. */
    struct np_presentation_clock historical = {.epoch = 13};
    struct np_presentation_clock latest = global;
    assert(np_presentation_clock_sample_at(&historical,
        100 * SECOND + 10 * MS, 100 * SECOND + 40 * MS,
        900 * SECOND + 25 * MS));
    expect_conversion(&historical, 900 * SECOND + 30 * MS,
        100 * SECOND + 300 * MS, 100 * SECOND + 30 * MS);
    expect_unchanged(&global, &latest);
}

static void overflow_and_future_times_are_rejected(void)
{
    struct np_presentation_clock clock = {0};
    assert(np_presentation_clock_sample_at(&clock,
        UINT64_MAX - 4 * MS, UINT64_MAX - 2 * MS, 100 * MS));
    assert(clock.guest == UINT64_MAX - 3 * MS);
    expect_conversion(&clock, 103 * MS, UINT64_MAX, UINT64_MAX);
    expect_rejection(&clock, 104 * MS, UINT64_MAX);

    clock = (struct np_presentation_clock){0};
    assert(np_presentation_clock_sample_at(&clock, MS, 3 * MS, SECOND));
    expect_rejection(&clock, SECOND - 2 * MS, 10 * MS); /* Exactly zero. */
    expect_rejection(&clock, SECOND - 3 * MS, 10 * MS); /* Negative. */

    clock = (struct np_presentation_clock){0};
    assert(np_presentation_clock_sample_at(&clock,
        SECOND, SECOND + 4 * MS, 10 * SECOND + 2 * MS));
    /* Permit only the half-RTT uncertainty, never an arbitrary future time. */
    expect_conversion(&clock, 10 * SECOND + 12 * MS,
        SECOND + 10 * MS, SECOND + 12 * MS);
    expect_rejection(&clock, 10 * SECOND + 12 * MS + 1, SECOND + 10 * MS);
    expect_rejection(&clock, 11 * SECOND, SECOND + 10 * MS);
}

static void post_pause_anchor_must_not_rewrite_historical_display(void)
{
    struct np_presentation_clock clock = {.epoch = 1, .epoch_host = 50 * SECOND + MS};
    assert(np_presentation_clock_sample_at(&clock,
        20 * SECOND, 20 * SECOND + 2 * MS, 50 * SECOND + MS));
    struct np_presentation_clock committed = clock;
    clock = (struct np_presentation_clock){
        .epoch = 2, .epoch_host = 60 * SECOND + 201 * MS,
    };
    assert(np_presentation_clock_sample_at(&clock,
        20 * SECOND + 200 * MS, 20 * SECOND + 202 * MS, 60 * SECOND + 201 * MS));
    assert(clock.epoch != committed.epoch);
    /* Applying the new offset to this old event used to return 10.05s,
     * passing the positive/not-future bounds despite being ten seconds wrong. */
    expect_rejection(&clock, 50 * SECOND + 50 * MS, 20 * SECOND + 300 * MS);
    expect_conversion(&committed, 50 * SECOND + 50 * MS,
        20 * SECOND + 300 * MS, 20 * SECOND + 50 * MS);
}

int main(void)
{
    different_origins_preserve_display_time();
    invalid_and_slow_samples_cannot_replace_calibration();
    fresh_calibration_keeps_the_lowest_rtt();
    vm_pause_requires_an_explicit_new_epoch();
    discontinuity_excludes_measured_transport_uncertainty();
    first_scene_keeps_its_actual_time_until_qualified();
    scene_sample_and_saved_anchor_are_independent_of_global_updates();
    overflow_and_future_times_are_rejected();
    post_pause_anchor_must_not_rewrite_historical_display();
    puts("Presentation clock origins, scene anchors, RTT selection, explicit epochs and bounds: PASS");
    return 0;
}
