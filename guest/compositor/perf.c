#include "perf.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct perf_counter {
	uint64_t count;
	uint64_t total_ns;
	uint64_t max_ns;
};

static struct perf_counter counters[NP_PERF_STAGE_COUNT];
static uint64_t report_start_ns;
static int enabled = -1;

static bool perf_enabled(void)
{
	if (enabled < 0)
		enabled = getenv("NP_PERF_TRACE") != NULL;
	return enabled != 0;
}

uint64_t np_perf_now_ns(void)
{
	if (!perf_enabled()) return 0;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void maybe_report(uint64_t now)
{
	if (!report_start_ns) {
		report_start_ns = now;
		return;
	}
	uint64_t interval = now - report_start_ns;
	if (interval < 1000000000ull) return;

	static const char *names[NP_PERF_STAGE_COUNT] = {
		"client-wait", "gpu-copy", "scene", "commit", "presented",
	};
	fprintf(stderr, "[perf] interval=%.3f", (double)interval / 1e9);
	for (int i = 0; i < NP_PERF_STAGE_COUNT; i++) {
		const struct perf_counter *counter = &counters[i];
		double average_ms = counter->count
			? (double)counter->total_ns / counter->count / 1e6 : 0.0;
		fprintf(stderr, " %s=%llu avg=%.3fms max=%.3fms",
		        names[i], (unsigned long long)counter->count,
		        average_ms, (double)counter->max_ns / 1e6);
	}
	fputc('\n', stderr);
	for (int i = 0; i < NP_PERF_STAGE_COUNT; i++)
		counters[i] = (struct perf_counter){0};
	report_start_ns = now;
}

void np_perf_record(enum np_perf_stage stage, uint64_t elapsed_ns)
{
	if (!perf_enabled() || stage >= NP_PERF_STAGE_COUNT) return;
	struct perf_counter *counter = &counters[stage];
	counter->count++;
	counter->total_ns += elapsed_ns;
	if (elapsed_ns > counter->max_ns) counter->max_ns = elapsed_ns;
	maybe_report(np_perf_now_ns());
}

void np_perf_count(enum np_perf_stage stage)
{
	if (!perf_enabled() || stage >= NP_PERF_STAGE_COUNT) return;
	counters[stage].count++;
	maybe_report(np_perf_now_ns());
}
