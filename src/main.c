#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <rte_eal.h>
#include <rte_cycles.h>
#include "scheduler.h"

/* Externing the operations tables from our modules */
extern const SchedulerOps sequential_scheduler_ops;
extern const SchedulerOps dispatcher_scheduler_ops;

/* Dummy callback for our benchmark tasks */
static void dummy_task_callback(void *arg) {
    uintptr_t val = (uintptr_t)arg;
    // Simulate a tiny workload (e.g., matrix or cipher element)
    volatile uint32_t dummy = 0;
    for (int i = 0; i < 50; i++) {
        dummy += val;
    }
}

/* Helper to convert hardware TSC cycles into human-readable nanoseconds */
static double cycles_to_ns(uint64_t cycles) {
    return ((double)cycles * 1000000000.0) / (double)rte_get_tsc_hz();
}

/* --- Traffic Generators --- */

static void run_periodic_benchmark(Scheduler *sched, uint32_t num_tasks, uint32_t interval_us) {
    printf("[Traffic Generator] Starting Periodic Traffic Profile (%d tasks, %dus interval)...\n", num_tasks, interval_us);
    
    uint64_t tsc_hz = rte_get_tsc_hz();
    uint64_t interval_cycles = (tsc_hz * interval_us) / 1000000;

    for (uint32_t i = 0; i < num_tasks; i++) {
        Task t = {
            .callback = dummy_task_callback,
            .arg = (void *)(uintptr_t)i
        };

        uint64_t start_ticks = rte_get_tsc_cycles();
        
        // Push to scheduler (lock-free)
        while (sched->ops->push_task(sched, t) < 0) {
            // If the queue is momentarily full, retry (busy wait for space)
            rte_pause();
        }

        // Wait until the next period interval
        while ((rte_get_tsc_cycles() - start_ticks) < interval_cycles) {
            rte_pause();
        }
    }
}

static void run_burst_benchmark(Scheduler *sched, uint32_t burst_size) {
    printf("[Traffic Generator] Starting Bursty Traffic Profile (Injecting %d tasks back-to-back)...\n", burst_size);
    
    for (uint32_t i = 0; i < burst_size; i++) {
        Task t = {
            .callback = dummy_task_callback,
            .arg = (void *)(uintptr_t)i
        };

        // Inject as fast as hardware allows
        if (sched->ops->push_task(sched, t) < 0) {
            // In burst mode, if queue overflows, we count it as dropped/failed packet
            // to see how well the system handles congestion.
        }
    }
}


static void print_scheduler_report(Scheduler *sched)
{
    SchedulerStats stats = {0};
    
    if (sched->ops->get_stats(sched, &stats, sizeof(SchedulerStats)) < 0) {
        printf("[Analyzer] Error: Failed to retrieve scheduler telemetry.\n");
        return;
    }

    printf("\n====================================================================================\n");
    printf("                                BENCHMARK PERFORMANCE REPORT                        \n");
    printf("====================================================================================\n");
    printf("%-10s | %-15s | %-18s | %-15s | %-15s\n", "Core ID", "Total Tasks", "Avg Latency(ns)", "Min Latency(ns)", "Max Latency(ns)");
    printf("------------------------------------------------------------------------------------\n");

    for (uint32_t i = 0; i < stats.valid_core_count; i++)
    {
        CoreStats *cs = &stats.per_core[i];
        double avg_ns = 0.0;
        
        if (cs->total_tasks > 0) {
            uint64_t avg_cycles = cs->total_latency_cycles / cs->total_tasks;
            avg_ns = cycles_to_ns(avg_cycles);
        }

        double min_ns = (cs->min_latency_cycles == UINT64_MAX) ? 0.0 : cycles_to_ns(cs->min_latency_cycles);
        double max_ns = cycles_to_ns(cs->max_latency_cycles);

        printf("Core %-5d | %-15llu | %-18.2f | %-15.2f | %-15.2f\n",
               stats.core_ids[i],
               (unsigned long long)cs->total_tasks,
               avg_ns,
               min_ns,
               max_ns);
    }
    printf("====================================================================================\n\n");
}


int main(int argc, char **argv)
{
    // 1. Initialize DPDK Environment Abstraction Layer (EAL)
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "[Fatal] Error with EAL initialization\n");
        return -1;
    }
    argc -= ret;
    argv += ret;

    printf("[Harness] DPDK EAL Initialized Successfully.\n");

    // 2. Define General Baseline Configurations
    uint32_t max_queue_size = 2048; // Must be power of 2 for rte_ring!
    
    SchedulerConfig base_config = {
        .target_cpu_core = 1, // Master core ID for sequential or dispatching loop
        .max_queue_size = max_queue_size,
        .enable_mem_lock = true,
        .strategy = WAITING_STRATEGY_PURE_POLLING // First test with pure polling
    };

    // ====================================================================================
    // STAGE 1: BENCHMARKING STRICT SEQUENTIAL SCHEDULER
    // ====================================================================================
    printf("\n>>> SCENARIO 1: Strict Sequential Scheduler <<<\n");
    Scheduler seq_sched = { .ops = &sequential_scheduler_ops };
    
    if (seq_sched.ops->init(&seq_sched, &base_config) == 0) {
        seq_sched.ops->start(&seq_sched);
        
        // Run both traffic profiles to see the sequential baseline
        run_periodic_benchmark(&seq_sched, 10000, 10); // 10k tasks, 10us apart
        run_burst_benchmark(&seq_sched, 1000);        // 1k tasks back-to-back
        
        seq_sched.ops->stop(&seq_sched);
        print_scheduler_report(&seq_sched);
    }

    // ====================================================================================
    // STAGE 2: BENCHMARKING DISPATCHER (SHARED RING - MPMC MODE)
    // ====================================================================================
    printf("\n>>> SCENARIO 2: Dispatcher Scheduler (Shared MPMC Ring) <<<\n");
    
    DispatcherExtendedConfig ext_shared_config = {
        .mode = DISPATCHER_MODE_SHARED_RING,
        .num_workers = 4,
        .worker_cores = {2, 3, 4, 5} // Assigned worker pool cores
    };
    
    SchedulerConfig disp_shared_config = base_config;
    disp_shared_config.extended_config = &ext_shared_config;
    
    Scheduler disp_shared_sched = { .ops = &dispatcher_scheduler_ops };
    
    if (disp_shared_sched.ops->init(&disp_shared_sched, &disp_shared_config) == 0) {
        disp_shared_sched.ops->start(&disp_shared_sched);
        
        run_periodic_benchmark(&disp_shared_sched, 10000, 10);
        run_burst_benchmark(&disp_shared_sched, 1000);
        
        disp_shared_sched.ops->stop(&disp_shared_sched);
        print_scheduler_report(&disp_shared_sched);
    }

    // ====================================================================================
    // STAGE 3: BENCHMARKING DISPATCHER (MULTI-RING - SPSC MODE)
    // ====================================================================================
    printf("\n>>> SCENARIO 3: Dispatcher Scheduler (Multi-Ring SPSC Per-Core) <<<\n");
    
    DispatcherExtendedConfig ext_multi_config = {
        .mode = DISPATCHER_MODE_MULTI_RING,
        .num_workers = 4,
        .worker_cores = {2, 3, 4, 5}
    };
    
    SchedulerConfig disp_multi_config = base_config;
    disp_multi_config.extended_config = &ext_multi_config;
    
    Scheduler disp_multi_sched = { .ops = &dispatcher_scheduler_ops };
    
    if (disp_multi_sched.ops->init(&disp_multi_sched, &disp_multi_config) == 0) {
        disp_multi_sched.ops->start(&disp_multi_sched);
        
        run_periodic_benchmark(&disp_multi_sched, 10000, 10);
        run_burst_benchmark(&disp_multi_sched, 1000);
        
        disp_multi_sched.ops->stop(&disp_multi_sched);
        print_scheduler_report(&disp_multi_sched);
    }

    // 3. Clean up DPDK EAL resource allocations
    rte_eal_cleanup();
    printf("[Harness] Benchmark Completed Successfully.\n");
    return 0;
}