/**
 * @file scheduler.h
 * @brief Public API for the multi-core scheduler engine.
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_WORKERS 16

/**
 * @brief Structural topologies to benchmark queue contention.
 */
typedef enum {
    TOPOLOGY_SHARED_QUEUE,       /* 1 Centralized MPMC Ring -> N Workers (N=1 is the Pipeline Baseline) */
    TOPOLOGY_DISTRIBUTED_QUEUES  /* N Dedicated SPSC Rings -> N Workers (Algos compete here) */
} QueueTopology;

/**
 * @brief Advanced routing algorithms to evaluate cache-locality and balance.
 */
typedef enum {
    ALGO_ROUND_ROBIN,
    ALGO_LEAST_LOADED,
    ALGO_FLOW_AFFINITY,
    ALGO_WORK_STEALING,
    ALGO_LOTTERY
} SchedulingAlgo;

/**
 * @brief Power/Latency balancing strategy for low-level core loops.
 */
typedef enum {
    WAITING_STRATEGY_PURE_POLLING,
    WAITING_STRATEGY_ADAPTIVE_YIELD
} WaitingStrategy;

/**
 * @brief Bounded task packet containing the workload payload.
 */
typedef struct {
    void (*callback)(void *);   /* Target workload function pointer */
    void *arg;                  /* Opaque payload argument pointer */
    uint64_t enqueue_tsc;       /* Hardware timestamp captured at push */
    uint32_t flow_id;           /* Flow identifier for affinity routing tests */
} Task;

/**
 * @brief Flat configuration matrix to easily orchestrate test variations.
 */
typedef struct {
    QueueTopology topology;
    SchedulingAlgo algo;
    WaitingStrategy strategy;
    uint32_t num_workers;
    int worker_cores[MAX_WORKERS];
    uint32_t max_queue_size;
} SchedulerConfig;

/* --- Core Synchronous Public API --- */

/**
 * @brief Allocates global DPDK infrastructure blocks based on topology.
 * @return 0 on success, -1 on failure.
 */
int scheduler_init(const SchedulerConfig *config);

/**
 * @brief Pushes a task copy into the pipeline using lock-free mechanisms.
 * @return 0 on success, -1 on failure (queue overflow).
 */
int scheduler_push(Task task);

/**
 * @brief Launches dedicated low-latency worker loops on specific CPU cores.
 * @return 0 on success, -1 on infrastructure failure.
 */
int scheduler_start(void);

/**
 * @brief Signals and safely synchronizes the shutdown sequence of all cores.
 */
void scheduler_stop(void);