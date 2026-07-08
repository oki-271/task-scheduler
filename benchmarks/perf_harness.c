/**
 * @file perf_harness.c
 * @brief High-frequency evaluation matrix testing core topologies and waiting strategies with zero runtime OS overhead.
 * @note All comments are strictly in English.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <rte_eal.h>
#include <rte_cycles.h>

#include "scheduler.h"
#include "profiler.h"

#define BURST_TEST_SIZE 5000
#define PERIODIC_TEST_SIZE 20000
#define PERIODIC_INTERVAL_US 50

/**
 * @brief Dummy workload execution target mapping telecom matrix computations.
 */
static void heavy_telecom_workload_callback(void *arg)
{
    uintptr_t val = (uintptr_t)arg;
    volatile uint32_t execution_sink = 0;

    for (int i = 0; i < 120; i++)
    {
        execution_sink += (uint32_t)(val + i);
    }
}

/**
 * @brief Injects deterministic constant bit rate traffic. ABSOLUTELY NO I/O ALLOWED INSIDE.
 */
static void run_periodic_traffic_profile(uint32_t num_tasks, uint32_t interval_us, const SchedulerConfig *config)
{
    uint64_t tsc_hz = rte_get_tsc_hz();
    uint64_t interval_cycles = (tsc_hz * interval_us) / 1000000;

    for (uint32_t i = 0; i < num_tasks; i++)
    {
        Task task = {
            .callback = heavy_telecom_workload_callback,
            .arg = (void *)(uintptr_t)i,
            .flow_id = i % (config->num_workers >> 1)};

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
static uint32_t run_burst_traffic_profile(uint32_t burst_size, const SchedulerConfig *config)
{
    uint32_t dropped_tasks = 0;

    for (uint32_t i = 0; i < burst_size; i++)
    {
        Task task = {
            .callback = heavy_telecom_workload_callback,
            .arg = (void *)(uintptr_t)i,
            .flow_id = i % (config->num_workers >> 1)};

        if (scheduler_push(task) < 0)
        {
            dropped_tasks++;
        }
    }
    return dropped_tasks;
}

/**
 * @brief Orchestrates isolated benchmark pipelines by sealing the execution window from OS interaction noise.
 */
static void execute_scenario(const char *scenario_name, const SchedulerConfig *config)
{
    if (scheduler_init(config) < 0)
    {
        fprintf(stderr, "[Fatal] Component initialization rejected structural constraints for: %s\n", scenario_name);
        return;
    }

    /* PRE-EXECUTION TIMELINE: Safe to execute formatting log statements */
    printf("\n====================================================================================\n");
    printf(" PREPARING SCENARIO: %s\n", scenario_name);
    printf("====================================================================================\n");
    printf("  -> Workers Configuration Count : %u\n", config->num_workers);
    printf("  -> Bounded Capacity Limit      : %u slots\n", config->max_queue_size);
    printf("  -> Core Execution Strategy     : %s\n",
           (config->strategy == WAITING_STRATEGY_PURE_POLLING) ? "PURE_POLLING" : "ADAPTIVE_YIELD");
    printf("  [Status] Booting isolated hardware cores... Zero-I/O active matrix window opens.\n");

    /* -----------------------------------------------------------------------------
     * CRITICAL: HOT-PATH EXECUTION WINDOW (ABSOLUTE SILENCE ZONE)
     * No printfs, no kernel logs, no dynamic changes between start and stop.
     * ----------------------------------------------------------------------------- */
    scheduler_start();

    run_periodic_traffic_profile(PERIODIC_TEST_SIZE, PERIODIC_INTERVAL_US, config);
    uint32_t dropped = run_burst_traffic_profile(BURST_TEST_SIZE, config);

    scheduler_stop();
    /* -----------------------------------------------------------------------------
     * POST-EXECUTION TIMELINE: Isolated hardware pipelines joined. Safe to resume I/O.
     * ----------------------------------------------------------------------------- */

    printf("  [Status] Target hardware loops synchronized and parked successfully.\n");
    if (dropped > 0)
    {
        printf("  [Alert] Queue saturation hit! Total structural overflow drops: %u\n", dropped);
    }
    else
    {
        printf("  [Status] Queue integrity sustained. Zero execution drops tracked.\n");
    }

    /* Dump collected telemetry matrix data */
    profiler_dump_results();
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

    /* ---------------------------------------------------------------------------------
     * SCENARIO 1: Baseline Pipeline Execution (1 Core, 1 Shared MPMC Queue - Pure Polling)
     * --------------------------------------------------------------------------------- */
    SchedulerConfig config_matrix = {
        .topology = TOPOLOGY_SHARED_QUEUE,
        .num_workers = 1,
        .worker_cores = {1},
        .max_queue_size = 4096,
        .strategy = WAITING_STRATEGY_PURE_POLLING};
    execute_scenario("1 Core, 1 Central Shared Queue (Pure Polling Baseline)", &config_matrix);

    /* ---------------------------------------------------------------------------------
     * SCENARIO 1.1: Baseline Pipeline Execution (1 Core, 1 Shared MPMC Queue - Adaptive Yield)
     * --------------------------------------------------------------------------------- */
    config_matrix.strategy = WAITING_STRATEGY_ADAPTIVE_YIELD;
    execute_scenario("1 Core, 1 Central Shared Queue (Adaptive Yield Baseline)", &config_matrix);

    /* ---------------------------------------------------------------------------------
     * SCENARIO 2: Lock Contention Evaluation (4 Cores, 1 Shared MPMC Queue - Pure Polling)
     * --------------------------------------------------------------------------------- */
    config_matrix = (SchedulerConfig){
        .topology = TOPOLOGY_SHARED_QUEUE,
        .num_workers = 4,
        .worker_cores = {1, 2, 3, 4},
        .max_queue_size = 4096,
        .strategy = WAITING_STRATEGY_PURE_POLLING};
    execute_scenario("4 Cores, 1 Central Shared Queue (Heavy MPMC Contention - Pure Polling)", &config_matrix);

    /* ---------------------------------------------------------------------------------
     * SCENARIO 2.1: Lock Contention Evaluation (4 Cores, 1 Shared MPMC Queue - Adaptive Yield)
     * --------------------------------------------------------------------------------- */
    config_matrix.strategy = WAITING_STRATEGY_ADAPTIVE_YIELD;
    execute_scenario("4 Cores, 1 Central Shared Queue (Heavy MPMC Contention - Adaptive Yield)", &config_matrix);

    /* ---------------------------------------------------------------------------------
     * SCENARIO 3: Distributed Isolated Execution (4 Cores, 4 SPSC Queues - Pure Polling)
     * --------------------------------------------------------------------------------- */
    config_matrix = (SchedulerConfig){
        .topology = TOPOLOGY_DISTRIBUTED_QUEUES,
        .algo = ALGO_ROUND_ROBIN,
        .num_workers = 4,
        .worker_cores = {1, 2, 3, 4},
        .max_queue_size = 4096,
        .strategy = WAITING_STRATEGY_PURE_POLLING};
    execute_scenario("4 Cores, 4 Distributed Queues (Lockless SPSC Round-Robin - Pure Polling)", &config_matrix);

    /* ---------------------------------------------------------------------------------
     * SCENARIO 3.1: Distributed Isolated Execution (4 Cores, 4 SPSC Queues - Adaptive Yield)
     * --------------------------------------------------------------------------------- */
    config_matrix.strategy = WAITING_STRATEGY_ADAPTIVE_YIELD;
    execute_scenario("4 Cores, 4 Distributed Queues (Lockless SPSC Round-Robin - Adaptive Yield)", &config_matrix);

    rte_eal_cleanup();
    printf("[Harness] All configuration matrix benchmark profiles executed clean.\n");
    return EXIT_SUCCESS;
}