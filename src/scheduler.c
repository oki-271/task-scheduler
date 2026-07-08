/**
 * @file scheduler.c
 * @brief Monolithic execution engine managing multi-core hardware routing paths.
 */

#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_cycles.h>
#include <rte_launch.h>
#include <stdio.h>
#include <string.h>

#include "scheduler.h"
#include "profiler.h"

/* Encapsulation structure for runtime parameters mapped inside the context helper */
typedef struct
{
    uint32_t worker_index;
} WorkerContext;

/* Monolithic global state to achieve absolute flat architecture memory access */
typedef struct
{
    struct rte_mempool *task_pool;
    struct rte_ring *shared_ring;               /* Active only during TOPOLOGY_SHARED_QUEUE */
    struct rte_ring *worker_rings[MAX_WORKERS]; /* Active during TOPOLOGY_DISTRIBUTED_QUEUES */

    SchedulerConfig config;
    volatile bool is_running;
    uint32_t rr_counter; /* Atomic distributor index counter */

    WorkerContext worker_ctx[MAX_WORKERS]; /* Pre-allocated immutable core context descriptors */
} GlobalSchedulerState;

static GlobalSchedulerState g_state;

/* --- Internal Worker Threads Fast-Path Loops --- */

/**
 * @file scheduler.c
 * @brief Helper and loop structures for distributed execution with work-stealing.
 */

static void worker_callback(Task *task_ptr, uint32_t worker_index)
{
    uint64_t start_tsc = task_ptr->enqueue_tsc;

    task_ptr->callback(task_ptr->arg);

    uint64_t latency = rte_get_tsc_cycles() - start_tsc;
    profiler_record_metric(worker_index, latency);

    rte_mempool_put(g_state.task_pool, task_ptr);
}

/**
 * @brief High-frequency loop for centralized competitive execution (MPMC path).
 */
static int worker_shared_queue_loop(void *arg)
{
    WorkerContext *ctx = (WorkerContext *)arg;
    uint32_t idx = ctx->worker_index;
    struct rte_ring *ring = g_state.shared_ring;

    while (__atomic_load_n(&g_state.is_running, __ATOMIC_RELAXED))
    {
        Task *task_ptr = NULL;

        if (rte_ring_dequeue(ring, (void **)&task_ptr) == 0)
        {
            worker_callback(task_ptr, idx);
            continue;
        }

        if (g_state.config.strategy == WAITING_STRATEGY_ADAPTIVE_YIELD)
        {
            rte_pause();
        }
    }
    return 0;
}

/**
 * @brief High-frequency loop for isolated distributed execution (SPSC path with Work-Stealing).
 */
static int worker_distributed_queue_loop(void *arg)
{
    WorkerContext *ctx = (WorkerContext *)arg;
    uint32_t idx = ctx->worker_index;
    struct rte_ring *my_ring = g_state.worker_rings[idx];

    while (__atomic_load_n(&g_state.is_running, __ATOMIC_RELAXED))
    {
        Task *task_ptr = NULL;

        if (rte_ring_dequeue(my_ring, (void **)&task_ptr) == 0)
        {
            worker_callback(task_ptr, idx);
            continue;
        }

        bool stolen = false;
        if (g_state.config.algo == ALGO_WORK_STEALING)
        {
            for (uint32_t i = 1; i < g_state.config.num_workers; ++i)
            {
                uint32_t offset = (i & 1) ? ((i >> 1) + 1) : (g_state.config.num_workers - (i >> 1));
                uint32_t neighbour_idx = (idx + offset) % g_state.config.num_workers;
                struct rte_ring *neighbour_ring = g_state.worker_rings[neighbour_idx];
                if (rte_ring_dequeue(neighbour_ring, (void **)&task_ptr) == 0)
                {
                    worker_callback(task_ptr, idx);
                    stolen = true;
                    break;
                }
            }
        }
        if (!stolen && g_state.config.strategy == WAITING_STRATEGY_ADAPTIVE_YIELD)
        {
            rte_pause();
        }
    }
    return 0;
}
/* --- Public API Core Implementation --- */

int scheduler_init(const SchedulerConfig *config)
{
    if (!config || config->num_workers > MAX_WORKERS)
    {
        return -1;
    }
    char name_buffer[64];

    /* Clear global container layout safely */
    memset(&g_state, 0, sizeof(GlobalSchedulerState));
    g_state.config = *config;
    g_state.is_running = false;

    /* 1. Allocate highly packed lock-free object memory pool */
    snprintf(name_buffer, sizeof(name_buffer), "GLOBAL_TASK_POOL_%p", (void *)&g_state);
    g_state.task_pool = rte_mempool_create(
        name_buffer, config->max_queue_size, sizeof(Task),
        0, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY,
        RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
    if (!g_state.task_pool)
        return -1;

    /* 2. Configure structures based on architectural topology definition */
    if (config->topology == TOPOLOGY_SHARED_QUEUE)
    {
        snprintf(name_buffer, sizeof(name_buffer), "CENTRAL_MPMC_RING_%p", (void *)&g_state);
        g_state.shared_ring = rte_ring_create(name_buffer, config->max_queue_size, SOCKET_ID_ANY, 0); // 0 flag enforces MPMC
        if (!g_state.shared_ring)
        {
            rte_mempool_free(g_state.task_pool);
            return -1;
        }
    }
    else
    {
        for (uint32_t i = 0; i < config->num_workers; i++)
        {
            snprintf(name_buffer, sizeof(name_buffer), "DIST_SPSC_RING_%p_%u", (void *)&g_state, i);
            g_state.worker_rings[i] = rte_ring_create(
                name_buffer, config->max_queue_size, SOCKET_ID_ANY,
                RING_F_SP_ENQ | RING_F_SC_DEQ // Hard SPSC flags for distributed isolation
            );
            if (!g_state.worker_rings[i])
            {
                /* Rollback previously allocated rings to avoid hanging zones */
                for (uint32_t j = 0; j < i; j++)
                {
                    rte_ring_free(g_state.worker_rings[j]);
                }
                rte_mempool_free(g_state.task_pool);
                return -1;
            }
        }
    }

    /* Initialize the telemetry structure to wipe raw memory state variables */
    profiler_init();
    return 0;
}

int scheduler_push(Task task)
{
    Task *task_ptr = NULL;
    if (rte_mempool_get(g_state.task_pool, (void **)&task_ptr) < 0)
    {
        return -1; /* Queue dropped due to systemic congestion */
    }

    *task_ptr = task;
    task_ptr->enqueue_tsc = rte_get_tsc_cycles();

    /* Routing layer selection */
    if (g_state.config.topology == TOPOLOGY_SHARED_QUEUE)
    {
        if (rte_ring_enqueue(g_state.shared_ring, task_ptr) < 0)
        {
            rte_mempool_put(g_state.task_pool, task_ptr);
            return -1;
        }
    }
    else
    {
        uint32_t worker_idx = 0;

        /* Execute target routing scheduling algorithm matrix selection */
        switch (g_state.config.algo)
        {
        case ALGO_ROUND_ROBIN:
        {
            uint32_t ticket = __atomic_fetch_add(&g_state.rr_counter, 1, __ATOMIC_RELAXED);
            worker_idx = ticket % g_state.config.num_workers;
            break;
        }
        case ALGO_LEAST_LOADED:
        {
            uint32_t min_ring_load = UINT32_MAX;
            worker_idx = 0;

            for (uint32_t i = 0; i < g_state.config.num_workers; ++i)
            {
                uint32_t ring_load = rte_ring_count(g_state.worker_rings[i]);
                if (ring_load < min_ring_load)
                {
                    min_ring_load = ring_load;
                    worker_idx = i;
                }
            }
            break;
        }
        case ALGO_FLOW_AFFINITY:
        {
            worker_idx = task_ptr->flow_id % g_state.config.num_workers;
        }
        case ALGO_LOTTERY:
        {
            worker_idx = ((task_ptr->enqueue_tsc) ^ (task_ptr->enqueue_tsc >> 4)) % g_state.config.num_workers;
        }
        default:
            /* Placeholders for our next custom implementations */
            worker_idx = 0;
            break;
        }

        if (rte_ring_enqueue(g_state.worker_rings[worker_idx], task_ptr) < 0)
        {
            rte_mempool_put(g_state.task_pool, task_ptr);
            return -1;
        }
    }
    return 0;
}

int scheduler_start(void)
{
    __atomic_store_n(&g_state.is_running, true, __ATOMIC_RELAXED);

    for (uint32_t i = 0; i < g_state.config.num_workers; i++)
    {
        g_state.worker_ctx[i].worker_index = i;

        int (*target_loop)(void *) = (g_state.config.topology == TOPOLOGY_SHARED_QUEUE) ? worker_shared_queue_loop : worker_distributed_queue_loop;

        int ret = rte_eal_remote_launch(target_loop, &g_state.worker_ctx[i], g_state.config.worker_cores[i]);
        if (ret < 0)
        {
            scheduler_stop();
            return -1;
        }
    }
    return 0;
}

void scheduler_stop(void)
{
    __atomic_store_n(&g_state.is_running, false, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < g_state.config.num_workers; i++)
    {
        rte_eal_wait_lcore(g_state.config.worker_cores[i]);
    }
}