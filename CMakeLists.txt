cmake_minimum_required(VERSION 3.10)
project(TaskScheduler C)

# Force C11 standard alignment
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler-specific configuration
if(MSVC)
    add_compile_options(/W4)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
else()
    # Linux GCC/Clang compiler flags
    add_compile_options(-Wall -Wextra -D_GNU_SOURCE)
endif()

# Include headers directory
include_directories(include)

# Define executable and sources
add_executable(task_scheduler
    src/main.c
    src/sequential_scheduler.c
)

# Link platform-specific or DPDK requirements here later if needed
# For now, keeping it minimal to pass compilation checks