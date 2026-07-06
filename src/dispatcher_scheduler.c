#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_lcore.h>

#include "scheduler.h"

typedef enum
{
    DISPATCHER_MODE_MULTI_RING, /* Path A: Dedicated SPSC ring per worker core */
    DISPATCHER_MODE_SHARED_RING /* Path B: Single centralized MPMC ring for all workers */
} DispatcherMode;

typedef struct
{
    DispatcherMode mode;
    uint32_t num_workers;
    int worker_cores[SCHEDULER_MAX_CORES]; /* Target cores for the worker pool */
} DispatcherExtendedConfig;

typedef struct
{
    void *data_ptr;        /* Pointer to global DispatcherData */
    uint32_t worker_index; /* Local index of this specific worker thread */
    CoreStats stats;       /* Isolated, non-atomic stats for this core */
} WorkerContext;

typedef struct
{
    struct rte_mempool *task_pool;                         /* Manages free Task objects in hugepages */
    struct rte_ring *ready_ring;                           /* Holds pointers to tasks pending execution */
    struct rte_ring *worker_rings[SCHEDULER_MAX_CORES]; /* Dedicated SPSC rings for each worker (Path A) */
    SchedulerConfig config;
    DispatcherExtendedConfig extended_config;
    volatile bool is_running; /* Controlled exit flag for the worker loop */

    uint32_t rr_counter; /* round-robin counter for strict load balancing */
    WorkerContext workerCtx[SCHEDULER_MAX_CORES];
} DispatcherData;

int dispatcher_init(void *self, const SchedulerConfig *config)
{
    if (!self || !config || !config->extended_config)
    {
        return -1;
    }
    char buff[64];
    Scheduler *generic_sched = (Scheduler *)self;

    // 1. Reserve memory zone for private data structure in hugepages
    snprintf(buff, sizeof(buff), "DISPATCHER_DATA_ZONE_%d", config->target_cpu_core);
    const struct rte_memzone *mz = rte_memzone_reserve(buff, sizeof(DispatcherData), SOCKET_ID_ANY, 0);
    if (mz == NULL)
    {
        return -1; /* Failed to reserve memory zone */
    }

    // 2. Create the lock-free mempool for reusable Task objects
    snprintf(buff, sizeof(buff), "DISPATCHER_TASK_POOL_NAME_%d", config->target_cpu_core);
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

    DispatcherData *data = (DispatcherData *)mz->addr;
    data->task_pool = mp;
    data->config = *config;
    data->extended_config = *(DispatcherExtendedConfig *)config->extended_config;
    data->is_running = false;
    data->rr_counter = 0;
    data->next_worker_index = 0;
    // 3. Create the rings based on the chosen mode
    if (data->extended_config.mode == DISPATCHER_MODE_MULTI_RING)
    {
        for (uint32_t i = 0; i < data->extended_config.num_workers; i++)
        {
            snprintf(buff, sizeof(buff), "DISP_W_RING_%p_%d", (void *)data, data->extended_config.worker_cores[i]);
            struct rte_ring *worker_ring = rte_ring_create(
                buff,
                config->max_queue_size,
                SOCKET_ID_ANY,
                RING_F_SP_ENQ | RING_F_SC_DEQ // SPSC optimized
            );

            if (worker_ring == NULL)
            {
                for (uint32_t j = 0; j < i; j++)
                {
                    rte_ring_free(data->worker_rings[j]);
                }
                rte_mempool_free((struct rte_mempool *)mp);
                rte_memzone_free(mz);
                return -1; /* Failed to create worker ring */
            }
            data->worker_rings[i] = worker_ring;
        }
    }
    else
    {
        snprintf(buff, sizeof(buff), "DISP_SH_RING_%p", (void *)data);
        struct rte_ring *ring = rte_ring_create(buff,
                                                config->max_queue_size,
                                                SOCKET_ID_ANY,
                                                0); /* Multi-producer / Multi-consumer optimization */
        if (ring == NULL)
        {
            rte_mempool_free((struct rte_mempool *)mp);
            rte_memzone_free(mz);
            return -1; /* Failed to create ring */
        }

        data->ready_ring = ring;
    }

    generic_sched->private_data = data;

    return 0;
}

static int dispatcher_worker_multi_loop(void *arg)
{
    WorkerContext *worker_ctx = (WorkerContext *)arg;
    DispatcherData *data = (DispatcherData *)worker_ctx->data_ptr;
    uint32_t worker_index = worker_ctx->worker_index;
    struct rte_ring *worker_ring = data->worker_rings[worker_index];

    worker_ctx->stats.min_latency_cycles = UINT64_MAX; // Initialize min latency to max possible value
    worker_ctx->stats.max_latency_cycles = 0;
    worker_ctx->stats.total_latency_cycles = 0;
    worker_ctx->stats.total_tasks = 0;

    while (__atomic_load_n(&data->is_running, __ATOMIC_RELAXED))
    {
        Task *task_ptr = NULL;

        // Strict SPSC dequeue from its own dedicated ring
        if (rte_ring_dequeue(worker_ring, (void **)&task_ptr) < 0)
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

            worker_ctx->stats.total_tasks++;
            worker_ctx->stats.total_latency_cycles += latency;

            if (latency > worker_ctx->stats.max_latency_cycles)
            {
                worker_ctx->stats.max_latency_cycles = latency;
            }
            if (latency < worker_ctx->stats.min_latency_cycles)
            {
                worker_ctx->stats.min_latency_cycles = latency;
            }

            /* Return the used Task object back to the mempool */
            rte_mempool_put(data->task_pool, task_ptr);
        }
    }
    return 0;
}

static int dispatcher_worker_shared_loop(void *arg)
{
    WorkerContext *worker_ctx = (WorkerContext *)arg;
    DispatcherData *data = (DispatcherData *)worker_ctx->data_ptr;
    struct rte_ring *shared_ring = data->ready_ring; // Using the central shared ring

    worker_ctx->stats.min_latency_cycles = UINT64_MAX; // Initialize min latency to max possible value
    worker_ctx->stats.max_latency_cycles = 0;
    worker_ctx->stats.total_latency_cycles = 0;
    worker_ctx->stats.total_tasks = 0;

    while (__atomic_load_n(&data->is_running, __ATOMIC_RELAXED))
    {
        Task *task_ptr = NULL;

        // Competitive MPMC dequeue where all workers fight for the same ring
        if (rte_ring_dequeue(shared_ring, (void **)&task_ptr) < 0)
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

            worker_ctx->stats.total_tasks++;
            worker_ctx->stats.total_latency_cycles += latency;

            if (latency > worker_ctx->stats.max_latency_cycles)
            {
                worker_ctx->stats.max_latency_cycles = latency;
            }
            if (latency < worker_ctx->stats.min_latency_cycles)
            {
                worker_ctx->stats.min_latency_cycles = latency;
            }

            /* Return the used Task object back to the mempool */
            rte_mempool_put(data->task_pool, task_ptr);
        }
    }
    return 0;
}

int dispatcher_start(void *self)
{
    if (!self)
    {
        return -1;
    }
    Scheduler *generic_sched = (Scheduler *)self;
    DispatcherData *data = (DispatcherData *)generic_sched->private_data;
    __atomic_store_n(&data->is_running, true, __ATOMIC_RELAXED);

    for(uint32_t i = 0; i < data->extended_config.num_workers; i++)
    {   
        data->workerCtx[i].data_ptr = data;
        data->workerCtx[i].worker_index = i;
        
        int (*worker_fn)(void *) = (data->extended_config.mode == DISPATCHER_MODE_MULTI_RING) ? 
                                    dispatcher_worker_multi_loop : dispatcher_worker_shared_loop;

        int ret = rte_eal_remote_launch(worker_fn, &data->workerCtx[i], data->extended_config.worker_cores[i]);
        if (ret < 0)
        {
            __atomic_store_n(&data->is_running, false, __ATOMIC_RELAXED);

            // Wait and sync with all previously launched worker cores in this cycle
            for (uint32_t j = 0; j < i; j++)
            {
                rte_eal_wait_lcore(data->extended_config.worker_cores[j]);
            }

            return -1; /* Core is busy or uninitialized */
        }
    }

    return 0; /* All workers booted successfully */
}

int dispatcher_stop(void *self)
{
    if (!self)
    {
        return -1;
    }
    Scheduler *generic_sched = (Scheduler *)self;
    DispatcherData *data = (DispatcherData *)generic_sched->private_data;
    __atomic_store_n(&data->is_running, false, __ATOMIC_RELAXED);

    for(uint32_t i = 0; i < data->extended_config.num_workers; i++)
    {   
        rte_eal_wait_lcore(data->extended_config.worker_cores[i]);
    }

    return 0;
}

int dispatcher_push(void *self, Task task)
{
    if (!self)
    {
        return -1;
    }
    Scheduler *generic_sched = (Scheduler *)self;
    DispatcherData *data = (DispatcherData *)generic_sched->private_data;

    // 1. Allocate a Task object from the mempool
    Task *task_ptr = NULL;
    if (rte_mempool_get(data->task_pool, (void **)&task_ptr) < 0)
    {
        return -1; /* Failed to allocate Task object */
    }

    // 2. Initialize the allocated Task object
    *task_ptr = task;
    task_ptr->enqueue_tsc = rte_get_tsc_cycles(); /* Capture enqueue timestamp */

    // 3. Enqueue the Task object based on the chosen mode
    if (data->extended_config.mode == DISPATCHER_MODE_MULTI_RING)
    {
        /* Path A: Dedicated SPSC ring per worker core */
        uint32_t current_count = __atomic_fetch_add(&data->rr_counter, 1, __ATOMIC_RELAXED);
        uint32_t worker_index = current_count % data->extended_config.num_workers; /* Simple round-robin distribution */
        if (rte_ring_enqueue(data->worker_rings[worker_index], task_ptr) < 0)
        {
            rte_mempool_put(data->task_pool, task_ptr); /* Return the Task object to the pool */
            return -1;                                  /* Failed to enqueue to the worker ring */
        }
    }
    else
    {
        /* Path B: Single centralized MPMC ring for all workers */
        if (rte_ring_enqueue(data->ready_ring, task_ptr) < 0)
        {
            rte_mempool_put(data->task_pool, task_ptr); /* Return the Task object to the pool */
            return -1;                                  /* Failed to enqueue to the shared ring */
        }
    }

    return 0;
}

int dispatcher_get_stats(void *self, void *stats_out, size_t stats_size)
{
    if (!self || !stats_out || stats_size < sizeof(SchedulerStats))
    {
        return -1;
    }

    Scheduler *generic_sched = (Scheduler *)self;
    DispatcherData *data = (DispatcherData *)generic_sched->private_data;
    SchedulerStats *out = (SchedulerStats *)stats_out;

    out->valid_core_count = data->extended_config.num_workers;

    for (uint32_t i = 0; i < data->extended_config.num_workers; i++)
    {
        out->core_ids[i] = data->extended_config.worker_cores[i];
        out->per_core[i] = data->workerCtx[i].stats;
    }

    return 0;
}


/* Export the Dispatcher Scheduler operations table */
const SchedulerOps dispatcher_scheduler_ops = {
    .init = dispatcher_init,
    .start = dispatcher_start,
    .stop = dispatcher_stop,
    .push_task = dispatcher_push,
    .get_stats = dispatcher_get_stats
};