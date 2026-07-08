cmake_minimum_required(VERSION 3.10)
project(TaskScheduler C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Feature Toggle: Control profiling via command line or CMake GUI
option(ENABLE_PROFILING_FEATURE "Toggle high performance profiling" ON)

if(ENABLE_PROFILING_FEATURE)
    add_compile_definitions(ENABLE_PROFILING)
endif()

# Global include paths for header access
include_directories(include)

# Platform-specific compiler flag configuration
if(MSVC)
    add_compile_options(/W4)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -O3 -D_GNU_SOURCE)
endif()

# Compile the core engine as a static reusable library
add_library(scheduler_core 
    src/scheduler.c 
    src/profiler.c
)

# Discover subdirectories for architectural isolation
add_subdirectory(benchmarks)
add_subdirectory(tests)