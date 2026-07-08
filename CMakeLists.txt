cmake_minimum_required(VERSION 3.10)
project(TaskScheduler C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Feature Toggle for live profiling telemetry
option(ENABLE_PROFILING_FEATURE "Toggle high performance profiling" ON)
if(ENABLE_PROFILING_FEATURE)
    add_compile_definitions(ENABLE_PROFILING)
endif()

# Strict modern compiler flags for Linux data-plane development
add_compile_options(-Wall -Wextra -O3 -D_GNU_SOURCE -march=native)

# Find DPDK system libraries dynamically using PkgConfig helper
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

# Enforce public and DPDK include directories layout
include_directories(
    include
    ${DPDK_INCLUDE_DIRS}
)

# Compile the core monolithic flat scheduler engine library
add_library(scheduler_core 
    src/scheduler.c 
    src/profiler.c
)

# Link hardware-specific internal DPDK compiler options to our core library
target_link_libraries(scheduler_core PRIVATE ${DPDK_LIBRARIES})
target_compile_options(scheduler_core PRIVATE ${DPDK_CFLAGS})

# Add native isolated subdirectories to build artifacts tree
add_subdirectory(benchmarks)
add_subdirectory(tests)