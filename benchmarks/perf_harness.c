/**
 * @file perf_harness.c
 * @brief High-frequency evaluation matrix testing core topologies, routing algorithms, 
 * and waiting strategies with REAL-WORLD cache thrashing simulation.
 * @note All comments are strictly in English.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <rte_eal.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_cycles.h>
#include <rte_launch.h>
#include "scheduler.h"
#include "profiler.h"

#define NUM_ACTIVE_FLOWS 8192  /* Large enough to thrash L1/L2 caches across cores */

/**
 * @brief Traffic Profile Weights to separate Latency and Throughput observations.
 */
typedef struct {
    const char *profile_name;
    uint32_t periodic_size;
    uint32_t periodic_interval_us;
    uint32_t burst_size;
    bool     burst_blocking; /* If true, master waits on full queue to measure max throughput */
} TrafficProfile;

/* PROFILE A: Latency Focused (90% Paced, 10% Micro-Burst) */
TrafficProfile PROFILE_LATENCY = {
    .profile_name = "LATENCY FOCUSED (Paced Traffic + Micro Bursts)",
    .periodic_size = 100000,
    .periodic_interval_us = 50,
    .burst_size = 10000,
    .burst_blocking = false /* Drop instantly if queue is full during micro-burst */
};

/* PROFILE B: Throughput Focused (10% Paced, 90% Heavy Saturation) */
TrafficProfile PROFILE_THROUGHPUT = {
    .profile_name = "THROUGHPUT FOCUSED (Max Saturation / Backpressure)",
    .periodic_size = 10000,
    .periodic_interval_us = 5,
    .burst_size = 150000,
    .burst_blocking = true /* Master spins until workers free the queue (Backpressure) */
};


/**
 * @brief Represents a real-world user session or bearer state.
 * Aligned to 64 bytes (Cache Line Size) to prevent False Sharing between cores.
 */
typedef struct {
    uint64_t packets_processed;
    uint64_t bytes_processed;
    uint32_t current_state_flag;
    uint8_t  payload_buffer[108]; /* Pad to exact 128 Bytes (2 Cache Lines) per flow */
} __attribute__((aligned(64))) FlowContext;

/* Global state datastore holding all active user contexts in RAM */
FlowContext g_flow_contexts[NUM_ACTIVE_FLOWS];

/**
 * @brief Real-world workload simulating packet parsing and state updates.
 * If a flow is bounced between cores, this will trigger severe L1/L2 Cache Misses!
 */
static void heavy_workload_callback(void *arg)
{
    uint32_t flow_id = (uint32_t)(uintptr_t)arg;
    if (flow_id >= NUM_ACTIVE_FLOWS) return;

    FlowContext *ctx = &g_flow_contexts[flow_id];

    /* Simulate packet header parsing and state modification */
    ctx->packets_processed++;
    ctx->current_state_flag ^= 0xDEADBEEF; /* Some bitwise state mutation */
    
    /* Touch multiple cache lines to ensure memory bus utilization and prevent optimization */
    for (int i = 0; i < 108; i++)
    {
        ctx->payload_buffer[i] = (uint8_t)((ctx->packets_processed + i) & 0xFF);
    }

    uint64_t tsc = rte_get_tsc_cycles();
    uint32_t random_jitter = ((tsc) ^ (tsc >> 4)) & 0x3F;
    uint32_t total_workload = 100 + random_jitter;

    volatile uint32_t dummy_math = 0;
    for (uint32_t i = 0; i < total_workload; i++) {
        dummy_math += (flow_id * i);
    }
    ctx->current_state_flag ^= dummy_math;
    ctx->bytes_processed += 64;
}

/**
 * @brief Injects deterministic constant bit rate traffic. ABSOLUTELY NO I/O ALLOWED INSIDE.
 */
static void run_periodic_traffic_profile(uint32_t num_tasks, uint32_t interval_us)
{
    uint64_t tsc_hz = rte_get_tsc_hz();
    uint64_t interval_cycles = (tsc_hz * interval_us) / 1000000;

    for (uint32_t i = 0; i < num_tasks; i++)
    {
        uint32_t flow_id = i % NUM_ACTIVE_FLOWS;
        Task task = {
            .callback = heavy_workload_callback,
            .arg = (void *)(uintptr_t)flow_id,
            .flow_id = flow_id
        };

        uint64_t start_ticks = rte_get_tsc_cycles();

        while (scheduler_push(task) < 0)
        {
            rte_pause();
        }

        while ((rte_get_tsc_cycles() - start_ticks) < interval_cycles)
        {
            rte_pause();
        }
    }
}

/**
 * @brief Forces unpaced saturation loads. Returns dropped count to keep hot-path silent.
 */
static uint32_t run_burst_traffic_profile(uint32_t burst_size, bool blocking)
{
    uint32_t dropped_tasks = 0;

    for (uint32_t i = 0; i < burst_size; i++)
    {
        uint32_t flow_id = (i * 7) % NUM_ACTIVE_FLOWS; /* Scatter flows pseudo-randomly */
        Task task = {
            .callback = heavy_workload_callback,
            .arg = (void *)(uintptr_t)flow_id,
            .flow_id = flow_id
        };

        if (blocking) {
            /* Throughput mode: Wait for workers to drain the queue (Backpressure) */
            while (scheduler_push(task) < 0) {
                rte_pause();
            }
        } else {
            /* Latency mode: Real network micro-burst drop behavior */
            if (scheduler_push(task) < 0) {
                dropped_tasks++;
            }
        }
    }
    return dropped_tasks;
}

/**
 * @brief Orchestrates isolated benchmark pipelines. Runs BOTH waiting strategies sequentially.
 */
static void execute_scenario(const char *scenario_name, SchedulerConfig *config)
{
    printf("\n\n");
    printf("████████████████████████████████████████████████████████████████████████████████████\n");
    printf("█ SCENARIO: %s\n", scenario_name);
    printf("████████████████████████████████████████████████████████████████████████████████████\n");
    printf("  => Topology   : %s\n", config->topology == TOPOLOGY_SHARED_QUEUE ? "SHARED MPMC" : "DISTRIBUTED SPSC");
    printf("  => Workers    : %u Cores\n", config->num_workers);
    printf("  => Queue Size : %u Slots\n", config->max_queue_size);

    WaitingStrategy strategies[] = {WAITING_STRATEGY_PURE_POLLING, WAITING_STRATEGY_ADAPTIVE_YIELD};
    const char *strategy_names[] = {"PURE POLLING (100% CPU Continuous Spin)", "ADAPTIVE YIELD (Hardware Pause / _mm_pause)"};

    TrafficProfile profiles[] = {PROFILE_LATENCY, PROFILE_THROUGHPUT};

    for (int s = 0; s < 2; s++)
    {
        config->strategy = strategies[s];
        
        for (int p = 0; p < 2; p++) 
        {
            TrafficProfile *prof = &profiles[p];

            if (scheduler_init(config) < 0) {
                fprintf(stderr, "  [Fatal] Component initialization rejected.\n");
                continue;
            }

            printf("\n");
            printf("  .------------------------------------------------------------------------------.\n");
            printf("  | STRATEGY : %-65s |\n", strategy_names[s]);
            printf("  | PROFILE  : %-65s |\n", prof->profile_name);
            printf("  '------------------------------------------------------------------------------'\n");


            memset(g_flow_contexts, 0, sizeof(g_flow_contexts));

            uint64_t exec_start_tsc = rte_get_tsc_cycles();
            
            (void)scheduler_start();
            run_periodic_traffic_profile(prof->periodic_size, prof->periodic_interval_us);
            uint32_t dropped = run_burst_traffic_profile(prof->burst_size, prof->burst_blocking);
            (void)scheduler_stop();
            
            uint64_t exec_end_tsc = rte_get_tsc_cycles();

            profiler_set_global_metrics(exec_start_tsc, exec_end_tsc, dropped);
            profiler_dump_results();
        }
    }
}

int main(int argc, char **argv)
{
    int eal_ret = rte_eal_init(argc, argv);
    if (eal_ret < 0)
    {
        fprintf(stderr, "[Fatal] DPDK Environment Abstraction Layer initialization failed.\n");
        return EXIT_FAILURE;
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
        perror("[Fatal] mlockall failed to lock process memory into RAM");
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }

    SchedulerConfig config_matrix;

    /* 1. Baseline (1 Core) */
    config_matrix = (SchedulerConfig){ .topology = TOPOLOGY_SHARED_QUEUE, .num_workers = 1, .worker_cores = {1}, .max_queue_size = 4096 };
    execute_scenario("[1] BASELINE (1 Core, Central Shared Queue)", &config_matrix);

    /* 2. Contention Evaluation (4 Cores) */
    config_matrix = (SchedulerConfig){ .topology = TOPOLOGY_SHARED_QUEUE, .num_workers = 4, .worker_cores = {1, 2, 3, 4}, .max_queue_size = 4096 };
    execute_scenario("[2] CONTENTION (4 Cores, Central Shared Queue)", &config_matrix);

    /* 3. Distributed - ROUND ROBIN */
    config_matrix = (SchedulerConfig){ .topology = TOPOLOGY_DISTRIBUTED_QUEUES, .algo = ALGO_ROUND_ROBIN, .num_workers = 4, .worker_cores = {1, 2, 3, 4}, .max_queue_size = 4096 };
    execute_scenario("[3] DISTRIBUTED - ROUND ROBIN", &config_matrix);

    /* 4. Distributed - LEAST LOADED */
    config_matrix.algo = ALGO_LEAST_LOADED;
    execute_scenario("[4] DISTRIBUTED - LEAST LOADED", &config_matrix);

    /* 5. Distributed - FLOW AFFINITY */
    config_matrix.algo = ALGO_FLOW_AFFINITY;
    execute_scenario("[5] DISTRIBUTED - FLOW AFFINITY (RSS / Zero Cache Miss)", &config_matrix);

    /* 6. Distributed - LOTTERY */
    config_matrix.algo = ALGO_LOTTERY;
    execute_scenario("[6] DISTRIBUTED - LOTTERY", &config_matrix);

    /* 7. Distributed - WORK STEALING */
    config_matrix.algo = ALGO_WORK_STEALING;
    execute_scenario("[7] DISTRIBUTED - WORK STEALING", &config_matrix);

    rte_eal_cleanup();
    printf("\n████████████████████████████████████████████████████████████████████████████████████\n");
    printf("█ [Harness] All Matrix tests executed completely.\n");
    printf("████████████████████████████████████████████████████████████████████████████████████\n\n");
    return EXIT_SUCCESS;
}