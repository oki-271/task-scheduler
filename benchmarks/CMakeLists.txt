# Define the performance executable
add_executable(perf_harness perf_harness.c)

# Link the core scheduling engine with aggressive optimizations
target_link_libraries(perf_harness scheduler_core)