/**
 * @file unit_tests.c
 * @brief Self-contained, zero-dependency unit testing harness verifying boundary states.
 */

#include <stdio.h>
#include <stdlib.h>
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

/* --- Mock Workload Callbacks --- */
static void mock_callback(void *arg) {
    (void)arg;
}

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
        .num_workers = 2,
        .worker_cores = {2, 3},
        .max_queue_size = 512,
        .strategy = WAITING_STRATEGY_PURE_POLLING
    };
    
    int status = scheduler_init(&valid_cfg);
    EXPECT_EQUAL(0, status, "scheduler_init must succeed under optimal validated operational structures");
}

/* --- Testing Entry Orchestration Point --- */
int main(void) {
    printf("===================================================================\n");
    printf("                  EXECUTION OF CORRECTNESS TESTS                   \n");
    printf("===================================================================\n");

    /* Fire isolated logical validation tests */
    RUN_TEST_CASE(test_init_rejects_null_config);
    RUN_TEST_CASE(test_init_rejects_excessive_workers);
    
    /* * Note: Running valid setup tests will expect DPDK resource allocations.
     * If the underlying hardware or mock drivers are absent, this may return failure.
     */
    RUN_TEST_CASE(test_init_accepts_valid_shared_config);

    printf("-------------------------------------------------------------------\n");
    printf("TEST SUITE SUMMARY: %d Executed | %d Passed | %d Failed\n", 
           g_tests_run, (g_tests_run - g_tests_failed), g_tests_failed);
    printf("===================================================================\n");

    return (g_tests_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}