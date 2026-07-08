#ifndef PROFILER_H
#define PROFILER_H

#include <stdint.h>
#include <stdbool.h>
#include "scheduler.h"

#define MAX_PROFILED_CORES SCHEDULER_MAX_CORES

typedef struct
{
    uint64_t total_tasks;
    uint64_t total_latency_cycles;
    uint64_t max_latency_cycles;
    uint64_t min_latency_cycles;
} CoreMetrics;

typedef struct
{
    CoreMetrics core_stats[MAX_PROFILED_CORES];
} ProfilerGlobalState;

extern ProfilerGlobalState g_profiler_state;

/* Public API control interface */
void profiler_init(void);
void profiler_dump_results(void);

static inline void profiler_record_metric(uint32_t core_id, uint64_t latency)
{
#ifdef ENABLE_PROFILING
    /* Bound check to guarantee safety inside low-level core mapping */
    if (core_id >= MAX_PROFILED_CORES)
    {
        return;
    }
    CoreMetrics *metrics = &g_profiler_state.core_stats[core_id];

    metrics->total_tasks++;
    metrics->total_latency_cycles += latency;

    if (latency > metrics->max_latency_cycles)
    {
        metrics->max_latency_cycles = latency;
    }
    if (latency < metrics->min_latency_cycles)
    {
        metrics->min_latency_cycles = latency;
    }
#else
    /* Compiles to absolute nothingness when ENABLE_PROFILING is undefined.
       Optimized out completely by the compiler pipeline. */
    (void)core_id;
    (void)latency;
#endif
}

#endif /* PROFILER_H */