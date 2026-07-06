#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_lcore.h>

#include "scheduler.h"

typedef struct
{
    struct rte_mempool *task_pool; /* Manages free Task objects in hugepages */
    struct rte_ring *ready_ring;   /* Holds pointers to tasks pending execution */
    SchedulerConfig config;
    volatile bool is_running; /* Controlled exit flag for the worker loop */
    SchedulerStats stats;   /* Isolated, non-atomic stats for this core */
} SequentialData;

static int seq_init(void *self, const SchedulerConfig *config)
{
    if (!self || !config)
    {
        return -1;
    }
    char buff[64];

    // 1. Reserve memory zone for private data structure in hugepages
    snprintf(buff, sizeof(buff), "SEQ_DATA_ZONE_%d", config->target_cpu_core);
    const struct rte_memzone *mz = rte_memzone_reserve(buff, sizeof(SequentialData), SOCKET_ID_ANY, 0);
    if (mz == NULL)
    {
        return -1; /* Failed to reserve memory zone */
    }

    // 2. Create the lock-free mempool for reusable Task objects
    snprintf(buff, sizeof(buff), "SEQ_TASK_POOL_NAME_%d", config->target_cpu_core);
    struct rte_mempool *mp = rte_mempool_create(buff,
                                                config->max_queue_size, /* Number of elements */
                                                sizeof(Task),           /* Size of each element */
                                                0,                      /* Per-lcore cache size (0 for simplicity) */
                                                0,                      /* Private data size */
                                                NULL, NULL,             /* Object constructor/destructor */
                                                NULL, NULL,             /* Obj arch pointer, socket_id */
                                                SOCKET_ID_ANY,
                                                RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET); /* Single-producer / Single-consumer optimization */

    if (mp == NULL)
    {
        rte_memzone_free(mz);
        return -1; /* Failed to create mempool */
    }

    // 3. Create the lock-free ring for signaling ready tasks
    snprintf(buff, sizeof(buff), "SEQ_READY_RING_NAME_%d", config->target_cpu_core);
    struct rte_ring *ring = rte_ring_create(buff,
                                            config->max_queue_size,
                                            SOCKET_ID_ANY,
                                            RTE_RING_F_SP_ENQ | RTE_RING_F_SC_DEQ); /* Single-producer / Single-consumer optimization */
    if (ring == NULL)
    {
        rte_mempool_free((struct rte_mempool *)mp);
        rte_memzone_free(mz);
        return -1; /* Failed to create ring */
    }

    SequentialData *data = (SequentialData *)mz->addr;
    data->task_pool = mp;
    data->ready_ring = ring;
    data->config = *config;

    Scheduler *generic_sched = (Scheduler *)self;
    generic_sched->private_data = data;

    return 0; /* Success */
}

static int seq_worker_loop(void *arg)
{
    SequentialData *data = (SequentialData *)arg;
    SchedulerStats *stats_ptr = (SchedulerStats *)&data->stats;
    CoreStats *core_stats = &stats_ptr->per_core[0]; /* Only one core in sequential scheduler */
    
    /* We will maintain a volatile run flag inside SequentialData later,
       for now let's assume a generic loop or a placeholder boolean */

    core_stats->min_latency_cycles = UINT64_MAX; // Initialize min latency to max possible value
    core_stats->max_latency_cycles = 0;
    core_stats->total_latency_cycles = 0;
    core_stats->total_tasks = 0;

    while (__atomic_load_n(&data->is_running, __ATOMIC_RELAXED))
    {
        Task *task_ptr = NULL;

        if (rte_ring_dequeue(data->ready_ring, (void **)&task_ptr) < 0)
        {
            if (data->config.strategy == WAITING_STRATEGY_ADAPTIVE_YIELD)
            {
                rte_pause(); /* Yield CPU to other threads */
            }
        }
        else
        {
            uint64_t current_tsc = rte_get_tsc_cycles();
            uint64_t latency = current_tsc - task_ptr->enqueue_tsc;

            /* Execute the callback */
            task_ptr->callback(task_ptr->arg);
            
            core_stats->total_tasks++;
            core_stats->total_latency_cycles += latency;

            if (latency > core_stats->max_latency_cycles)
            {
                core_stats->max_latency_cycles = latency;
            }
            if (latency < core_stats->min_latency_cycles)
            {
                core_stats->min_latency_cycles = latency;
            }

            /* Return the used Task object back to the mempool */
            rte_mempool_put(data->task_pool, task_ptr);
        }
    }

    return 0;
}

static int seq_start(void *self)
{
    if (!self)
    {
        return -1;
    }
    Scheduler *generic_sched = (Scheduler *)self;
    SequentialData *data = (SequentialData *)generic_sched->private_data;
    __atomic_store_n(&data->is_running, true, __ATOMIC_RELAXED);

    int ret = rte_eal_remote_launch(seq_worker_loop, data, data->config.target_cpu_core);
    if (ret < 0)
    {
        __atomic_store_n(&data->is_running, false, __ATOMIC_RELAXED);
        return -1; /* Core is busy or uninitialized */
    }

    return 0;
}

static int seq_stop(void *self)
{
    if (!self)
    {
        return -1;
    }
    Scheduler *generic_sched = (Scheduler *)self;
    SequentialData *data = (SequentialData *)generic_sched->private_data;
    __atomic_store_n(&data->is_running, false, __ATOMIC_RELAXED);
    rte_eal_wait_lcore(data->config.target_cpu_core); /* Wait for the worker loop to exit */
    return 0;
}

static int seq_push_task(void *self, Task task)
{
    if (!self)
    {
        return -1;
    }
    Scheduler *generic_sched = (Scheduler *)self;
    SequentialData *data = (SequentialData *)generic_sched->private_data;

    // 1. Allocate a Task object from the mempool
    Task *task_ptr = NULL;
    if (rte_mempool_get(data->task_pool, (void **)&task_ptr) < 0)
    {
        return -1; /* Failed to allocate Task object */
    }

    // 2. Initialize the allocated Task object
    *task_ptr = task;
    task_ptr->enqueue_tsc = rte_get_tsc_cycles(); /* Capture enqueue timestamp */

    // 3. Add the Task object to the ready ring
    if (rte_ring_enqueue(data->ready_ring, task_ptr) < 0)
    {
        rte_mempool_put(data->task_pool, task_ptr);
        return -1; /* Failed to enqueue Task object */
    }

    return 0; /* Success */
}

int seq_get_stats(void *self, void *stats_out, size_t stats_size)
{
    if (!self || !stats_out || stats_size < sizeof(SchedulerStats))
    {
        return -1;
    }
    Scheduler *generic_sched = (Scheduler *)self;
    SequentialData *data = (SequentialData *)generic_sched->private_data;
    CoreStats *core_stats = &data->stats.per_core[0];
    SchedulerStats *out = (SchedulerStats *)stats_out;

    out->valid_core_count = 1;
    out->core_ids[0] = data->config.target_cpu_core;
    out->per_core[0] = *core_stats; /* Copy the core stats */

    return 0; /* Success */
}

/* Export the Sequential Scheduler operations table */
const SchedulerOps sequential_scheduler_ops = {
    .init      = seq_init,
    .start     = seq_start,
    .stop      = seq_stop,
    .push_task = seq_push_task,
    .get_stats = seq_get_stats
};