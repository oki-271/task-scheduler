/**
 * @file perf_harness.c
 * @brief High-frequency performance evaluation arena simulating real-time telecom traffic profiles.
 */

#include <stdio.h>
#include <stdlib.h>
#include <rte_eal.h>
#include <rte_cycles.h>

#include "scheduler.h"
#include "profiler.h"

#define BURST_TEST_SIZE   5000
#define PERIODIC_TEST_SIZE 20000
#define PERIODIC_INTERVAL_US 50 /* Simulating a tight 50-microsecond periodic cycle */

/**
 * @brief Dummy workload execution target mapping telecom matrix computations.
 */
static void heavy_telecom_workload_callback(void *arg) {
    uintptr_t val = (uintptr_t)arg;
    volatile uint32_t execution_sink = 0;
    
    /* Simulate a realistic radio frame/symbol signal processing delay profile */
    for (int i = 0; i < 120; i++) {
        execution_sink += (uint32_t)(val + i);
    }
}

/**
 * @brief Injects deterministic constant bit rate traffic intervals into the active system topology.
 */
static void run_periodic_traffic_profile(uint32_t num_tasks, uint32_t interval_us) {
    printf("[Traffic Generator] Starting Periodic Profile: %u tasks, %u us intervals...\n", num_tasks, interval_us);
    
    uint64_t tsc_hz = rte_get_tsc_hz();
    uint64_t interval_cycles = (tsc_hz * interval_us) / 1000000;

    for (uint32_t i = 0; i < num_tasks; i++) {
        Task task = {
            .callback = heavy_telecom_workload_callback,
            .arg = (void *)(uintptr_t)i,
            .flow_id = i % 4 /* Spreading traffic across 4 distinct virtual user profiles */
        };

        uint64_t start_ticks = rte_get_tsc_cycles();
        
        /* Spin-lock push until the internal ring structures accept the entry */
        while (scheduler_push(task) < 0) {
            rte_pause();
        }

        /* Enforce tight temporal precision pacing link */
        while ((rte_get_tsc_cycles() - start_ticks) < interval_cycles) {
            rte_pause();
        }
    }
}

/**
 * @brief Forces sudden, unpaced saturation loads to stress limits and measure queue bottlenecks.
 */
static void run_burst_traffic_profile(uint32_t burst_size) {
    printf("[Traffic Generator] Starting Congestion Burst Profile: Injecting %u tasks back-to-back...\n", burst_size);
    
    uint32_t dropped_tasks = 0;
    
    for (uint32_t i = 0; i < burst_size; i++) {
        Task task = {
            .callback = heavy_telecom_workload_callback,
            .arg = (void *)(uintptr_t)i,
            .flow_id = i % 4
        };

        /* In back-to-back stress profiles, treat target ring full limits as packet drops */
        if (scheduler_push(task) < 0) {
            dropped_tasks++;
        }
    }
    
    if (dropped_tasks > 0) {
        printf("[Traffic Alert] Queue saturation reached. Structural dropped items count: %u\n", dropped_tasks);
    }
}

/**
 * @brief Orchestrates isolated end-to-end benchmark scenario pipelines.
 */
static void execute_scenario(const char *scenario_name, const SchedulerConfig *config) {
    printf("\n>>> SCENARIO START: %s <<<\n", scenario_name);
    
    if (scheduler_init(config) < 0) {
        fprintf(stderr, "[Fatal] Component initialization rejected structural constraints.\n");
        return;
    }
    
    if (scheduler_start() < 0) {
        fprintf(stderr, "[Fatal] Thread execution launch pipeline broke down.\n");
        return;
    }

    /* 1. Fire Constant Bit Rate Profile */
    run_periodic_traffic_profile(PERIODIC_TEST_SIZE, PERIODIC_INTERVAL_US);
    
    /* 2. Fire Sudden Saturation Congestion Surge */
    run_burst_traffic_profile(BURST_TEST_SIZE);

    /* Clean down active worker threads before pulling telemetry metrics maps */
    scheduler_stop();
    
    /* Extract isolated analytical profile results reports */
    profiler_dump_results();
}

int main(int argc, char **argv) {
    int eal_ret = rte_eal_init(argc, argv);
    if (eal_ret < 0) {
        fprintf(stderr, "[Fatal] DPDK Environment Abstraction Layer initialization failed.\n");
        return EXIT_FAILURE;
    }

    /* ---------------------------------------------------------------------------------
     * BENCHMARK MATRIX ENTRY 1: Baseline Pipeline Execution (1 Core, 1 Shared MPMC Queue)
     * --------------------------------------------------------------------------------- */
    SchedulerConfig baseline_config = {
        .topology = TOPOLOGY_SHARED_QUEUE,
        .num_workers = 1,
        .worker_cores = {1},
        .max_queue_size = 4096,
        .strategy = WAITING_STRATEGY_PURE_POLLING
    };
    execute_scenario("1 Core, 1 Central Shared Queue Pure Polling (Absolute Baseline)", &baseline_config);

    /* ---------------------------------------------------------------------------------
     * BENCHMARK MATRIX ENTRY 1.1: Baseline Pipeline Execution (1 Core, 1 Shared MPMC Queue)
     * --------------------------------------------------------------------------------- */
    baseline_config = {
        .topology = TOPOLOGY_SHARED_QUEUE,
        .num_workers = 1,
        .worker_cores = {1},
        .max_queue_size = 4096,
        .strategy = WAITING_STRATEGY_ADAPTIVE_YIELD
    };
    execute_scenario("1 Core, 1 Central Shared Queue Adaptive Yield (Absolute Baseline)", &baseline_config);

    /* ---------------------------------------------------------------------------------
     * BENCHMARK MATRIX ENTRY 2: Lock Contention Evaluation (4 Cores, 1 Shared MPMC Queue)
     * --------------------------------------------------------------------------------- */
    SchedulerConfig contention_config = {
        .topology = TOPOLOGY_SHARED_QUEUE,
        .worker_cores = {1, 2, 3, 4},
        .strategy = WAITING_STRATEGY_PURE_POLLING
    };
    execute_scenario("4 Cores, 1 Central Shared Queue Pure Polling (Heavy MPMC Contention)", &contention_config);

    /* ---------------------------------------------------------------------------------
     * BENCHMARK MATRIX ENTRY 2.1: Lock Contention Evaluation (4 Cores, 1 Shared MPMC Queue)
     * --------------------------------------------------------------------------------- */
    contention_config = {
        .topology = TOPOLOGY_SHARED_QUEUE,
        .num_workers = 4,
        .worker_cores = {1, 2, 3, 4},
        .max_queue_size = 4096,
        .strategy = WAITING_STRATEGY_ADAPTIVE_YIELD
    };
    execute_scenario("4 Cores, 1 Central Shared Queue Adaptive Yield (Heavy MPMC Contention)", &contention_config);

    /* ---------------------------------------------------------------------------------
     * BENCHMARK MATRIX ENTRY 3: Distributed Isolated Execution (4 Cores, 4 SPSC Queues - RR)
     * --------------------------------------------------------------------------------- */
    SchedulerConfig parallel_rr_config = {
        .topology = TOPOLOGY_DISTRIBUTED_QUEUES,
        .algo = ALGO_ROUND_ROBIN,
        .num_workers = 4,
        .worker_cores = {1, 2, 3, 4},
        .max_queue_size = 4096,
        .strategy = WAITING_STRATEGY_PURE_POLLING
    };
    execute_scenario("4 Cores, 4 Dedicated Distributed Queues Pure Polling (Lockless SPSC Round-Robin)", &parallel_rr_config);

    /* ---------------------------------------------------------------------------------
     * BENCHMARK MATRIX ENTRY 3.1: Distributed Isolated Execution (4 Cores, 4 SPSC Queues - RR)
     * --------------------------------------------------------------------------------- */
    parallel_rr_config = {
        .topology = TOPOLOGY_DISTRIBUTED_QUEUES,
        .algo = ALGO_ROUND_ROBIN,
        .num_workers = 4,
        .worker_cores = {1, 2, 3, 4},
        .max_queue_size = 4096,
        .strategy = WAITING_STRATEGY_ADAPTIVE_YIELD
    };
    execute_scenario("4 Cores, 4 Dedicated Distributed Queues Adaptive Yield (Lockless SPSC Round-Robin)", &parallel_rr_config);

    rte_eal_cleanup();
    printf("[Harness] All system benchmark evaluation matrix lines executed clean.\n");
    return EXIT_SUCCESS;
}