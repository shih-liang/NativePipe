#ifndef NATIVEPIPE_PERF_H
#define NATIVEPIPE_PERF_H

#include <stdint.h>

enum np_perf_stage {
	NP_PERF_CLIENT_WAIT,
	NP_PERF_GPU_COPY,
	NP_PERF_SCENE_COMPOSE,
	NP_PERF_COMMIT_SENT,
	NP_PERF_PRESENTED,
	NP_PERF_STAGE_COUNT,
};

uint64_t np_perf_now_ns(void);
void np_perf_record(enum np_perf_stage stage, uint64_t elapsed_ns);
void np_perf_count(enum np_perf_stage stage);

#endif
