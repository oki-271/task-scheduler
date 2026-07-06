#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>

#define SCHEDULER_MAX_CORES 16

typedef struct {
    uint64_t total_tasks;
    uint64_t total_latency_cycles;
    uint64_t max_latency_cycles;
    uint64_t min_latency_cycles;
} CoreStats;

typedef struct {
    uint32_t valid_core_count;
    int core_ids[SCHEDULER_MAX_CORES];
    CoreStats per_core[SCHEDULER_MAX_CORES];
} SchedulerStats;

/* 1. Generic Task Representation */
typedef struct {
    void (*callback)(void *arg);  /* Function pointer to execute */
    void *arg;                    /* Context/Arguments for the callback */
    uint64_t enqueue_tsc;     /* Timestamp captured when task enters the scheduler */
} Task;

/* 2. Generic Configuration Representation */
typedef enum {
    WAITING_STRATEGY_PURE_POLLING,
    WAITING_STRATEGY_ADAPTIVE_YIELD
} WaitingStrategy;

typedef struct {
    int target_cpu_core;        /* Dedicated core ID for thread affinity pinning */
    uint32_t max_queue_size;    /* Bounded size for internal tracking ring buffers */
    bool enable_mem_lock;       /* Toggle to execute mlockall on start */
    WaitingStrategy strategy;   /* Strategy for the worker loop when queue is empty */
    
    void *extended_config;      /* Extension hook for specific implementation parameters */
} SchedulerConfig;

/* 3. The Operation Table */
typedef struct SchedulerOps {
    /**
     * @brief Allocates and initializes inner structures using generic configuration.
     * @param self Pointer to top-level context wrapper.
     * @param config Hand-shake config parameters.
     */
    int (*init)(void *self, const SchedulerConfig *config);
    int (*start)(void *self);
    int (*stop)(void *self);
    int (*push_task)(void *self, Task task);
    int (*get_stats)(void *self, void *stats_out, size_t stats_size);
} SchedulerOps;

/* 4. The Top-Level Context Wrapper */
typedef struct {
    const SchedulerOps *ops;     /* Pointer to the active behavior table */
    void *private_data;         /* Pointer to the concrete scheduler memory block */
} Scheduler;

#endif /* SCHEDULER_H */