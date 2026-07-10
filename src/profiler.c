/**
 * @file profiler.c
 * @brief Out-of-path heavy reporting logic for telemetry data.
 */

#include <stdio.h>
#include <string.h>
#include <rte_cycles.h>
#include "profiler.h"

/* Instantiate the global state allocated for fast cache-line access */
ProfilerGlobalState g_profiler_state;

/**
 * @brief Helper utility to convert hardware TSC ticks into fractional nanoseconds.
 */
static double tsc_to_nanoseconds(uint64_t cycles) {
    uint64_t hz = rte_get_tsc_hz();
    if (hz == 0) return 0.0;
    return ((double)cycles * 1000000000.0) / (double)hz;
}

void profiler_init(void) {
    /* Explicitly wipe the memory zone to guarantee clean baseline telemetry metrics */
    memset(&g_profiler_state, 0, sizeof(ProfilerGlobalState));
    
    for (uint32_t i = 0; i < MAX_PROFILED_CORES; i++) {
        g_profiler_state.core_stats[i].min_latency_cycles = UINT64_MAX;
    }
}

void profiler_dump_results(void) {
    // printf("\n====================================================================================\n");
    // printf("                       AUTOMATED PROFILER ARCHITECTURAL REPORT                     \n");
    // printf("====================================================================================\n");

#ifndef ENABLE_PROFILING
    printf("[Profiler Notice] Telemetry is currently SHUT DOWN via compiler macro switches.\n");
    printf("                  To unlock live stats, define ENABLE_PROFILING during compilation.\n");
    printf("====================================================================================\n\n");
    return;
#else
    printf("%-10s | %-15s | %-18s | %-15s | %-15s\n", 
           "Core Index", "Total Packets", "Avg Latency(ns)", "Min Latency(ns)", "Max Latency(ns)");
    printf("------------------------------------------------------------------------------------\n");

    uint64_t grand_total_tasks = 0;
    uint64_t sys_total_cycles = 0;
    uint64_t sys_min_cycles = UINT64_MAX;
    uint64_t sys_max_cycles = 0;
    uint32_t active_core_count = 0;

    for (uint32_t i = 0; i < MAX_PROFILED_CORES; i++) {
        CoreMetrics *cm = &g_profiler_state.core_stats[i];
        
        /* Skip unallocated or inactive core slots to keep output clean */
        if (cm->total_tasks == 0) {
            continue;
        }

        grand_total_tasks += cm->total_tasks;
        sys_total_cycles += cm->total_latency_cycles;
        sys_min_cycles += cm->min_latency_cycles;
        sys_max_cycles += cm->max_latency_cycles;
        active_core_count++;

        double avg_ns = tsc_to_nanoseconds(cm->total_latency_cycles / cm->total_tasks);
        double min_ns = (cm->min_latency_cycles == UINT64_MAX) ? 0.0 : tsc_to_nanoseconds(cm->min_latency_cycles);
        double max_ns = tsc_to_nanoseconds(cm->max_latency_cycles);

        printf("Worker %-3u | %-15llu | %-18.2f | %-15.2f | %-15.2f\n",
               i,
               (unsigned long long)cm->total_tasks,
               avg_ns,
               min_ns,
               max_ns);
    }

    if (grand_total_tasks > 0 && active_core_count > 1) {
        double sys_avg_ns = tsc_to_nanoseconds(sys_total_cycles / grand_total_tasks);
        double sys_min_ns = tsc_to_nanoseconds(sys_min_cycles / active_core_count);
        double sys_max_ns = tsc_to_nanoseconds(sys_max_cycles / active_core_count);

        printf("------------------------------------------------------------------------------------\n");
        printf("SYSTEM AVG | %-15llu | %-18.2f | %-15.2f | %-15.2f\n",
               (unsigned long long)grand_total_tasks, sys_avg_ns, sys_min_ns, sys_max_ns);
               
        /* MPPS and Make-Span calculation */
        if (g_profiler_state.global_end_tsc > g_profiler_state.global_start_tsc) {
            uint64_t total_exec_cycles = g_profiler_state.global_end_tsc - g_profiler_state.global_start_tsc;
            double total_exec_time_sec = (double)total_exec_cycles / (double)rte_get_tsc_hz();
            double mpps = ((double)grand_total_tasks / total_exec_time_sec) / 1000000.0;
            
            printf("====================================================================================\n");
            printf("  [Metrics] Total Execution Time : %.6f seconds\n", total_exec_time_sec);
            printf("  [Metrics] Throughput (MPPS)    : %.3f Million Packets/Sec\n", mpps);
            
            if (g_profiler_state.global_dropped_tasks > 0) {
                printf("  [Alert] Dropped Packets        : %u (Queue Saturation)\n", g_profiler_state.global_dropped_tasks);
            }
        }
    }
    
    // printf("====================================================================================\n\n");

    profiler_init();
    profiler_set_global_metrics(0,0,0);
#endif
}