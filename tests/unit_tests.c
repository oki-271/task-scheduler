/**
 * @file unit_tests.c
 * @brief Self-contained, zero-dependency unit testing harness verifying boundary states.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rte_eal.h>

#include "scheduler.h"

/* --- Ultra-Lightweight Inline Micro Testing Framework --- */
static int g_tests_run = 0;
static int g_tests_failed = 0;

#define EXPECT_EQUAL(expected, actual, message) do { \
    g_tests_run++; \
    if ((expected) != (actual)) { \
        printf("  [FAIL] %s -> Expected %d, got %d (Line %d)\n", message, (int)(expected), (int)(actual), __LINE__); \
        g_tests_failed++; \
    } else { \
        printf("  [PASS] %s\n", message); \
    } \
} while(0)

#define RUN_TEST_CASE(test_func) do { \
    printf("[Test Case] Running %s...\n", #test_func); \
    test_func(); \
    printf("\n"); \
} while(0)

/* --- Concrete Test Suite Operations --- */

/**
 * @brief Verifies that passing NULL configurations returns safe error codes.
 */
static void test_init_rejects_null_config(void) {
    int status = scheduler_init(NULL);
    EXPECT_EQUAL(-1, status, "scheduler_init must fail gracefully when config is NULL");
}

/**
 * @brief Verifies that configuring more workers than MAX_WORKERS is intercepted safely.
 */
static void test_init_rejects_excessive_workers(void) {
    SchedulerConfig invalid_cfg = {
        .topology = TOPOLOGY_SHARED_QUEUE,
        .num_workers = MAX_WORKERS + 5, /* Exceeds hard upper bound boundary */
        .max_queue_size = 1024
    };
    
    int status = scheduler_init(&invalid_cfg);
    EXPECT_EQUAL(-1, status, "scheduler_init must block initialization when num_workers exceeds MAX_WORKERS");
}

/**
 * @brief Verifies correct functional setup behavior under valid standard parameters.
 */
static void test_init_accepts_valid_shared_config(void) {
    SchedulerConfig valid_cfg = {
        .topology = TOPOLOGY_SHARED_QUEUE,
        .num_workers = 1,
        .worker_cores = {1},
        .max_queue_size = 256,
        .strategy = WAITING_STRATEGY_PURE_POLLING
    };
    
    int status = scheduler_init(&valid_cfg);
    EXPECT_EQUAL(0, status, "scheduler_init must succeed under optimal validated operational structures");
}

static void test_start_and_stop(void) {
    int start_status = scheduler_start();
    EXPECT_EQUAL(0, start_status, "scheduler_start must succeed under valid configuration");

    int stop_status = scheduler_stop();
    EXPECT_EQUAL(0, stop_status, "scheduler_stop must succeed after starting the scheduler");
}

/* --- Testing Entry Orchestration Point --- */
int main(int argc, char **argv) {
    printf("===================================================================\n");
    printf("                  EXECUTION OF CORRECTNESS TESTS                   \n");
    printf("===================================================================\n");

    int eal_ret = rte_eal_init(argc, argv);
    if (eal_ret < 0) {
        fprintf(stderr, "[Fatal] DPDK EAL initialization failed in unit tests.\n");
        return EXIT_FAILURE;
    }

    /* Fire isolated logical validation tests */
    RUN_TEST_CASE(test_init_rejects_null_config);
    RUN_TEST_CASE(test_init_rejects_excessive_workers);
    RUN_TEST_CASE(test_init_accepts_valid_shared_config);
    RUN_TEST_CASE(test_start_and_stop);


    printf("-------------------------------------------------------------------\n");
    printf("TEST SUITE SUMMARY: %d Executed | %d Passed | %d Failed\n", 
           g_tests_run, (g_tests_run - g_tests_failed), g_tests_failed);
    printf("===================================================================\n");

    rte_eal_cleanup();
    return (g_tests_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}