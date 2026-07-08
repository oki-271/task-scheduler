# Define the correctness testing executable
add_executable(unit_tests unit_tests.c)

# Link the correctness module with the core library
target_link_libraries(unit_tests scheduler_core)

# Register with CMake's built-in ctest driver tool
enable_testing()
add_test(NAME CorrectnessBoundaryTests COMMAND unit_tests)