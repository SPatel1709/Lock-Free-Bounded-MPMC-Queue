cmake_minimum_required(VERSION 3.16)
project(lock_free_mpmc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(EXPERIMENT_ITEMS 1000000 CACHE STRING "Items transferred in each benchmark scenario")
set(EXPERIMENT_RUNS 5 CACHE STRING "Measured runs per benchmark scenario")

find_package(Threads REQUIRED)
enable_testing()

add_executable(correctness tests/correctness.cpp)
target_include_directories(correctness PRIVATE include)
target_link_libraries(correctness PRIVATE Threads::Threads)
add_test(NAME test_correctness COMMAND correctness)

add_executable(stress_test tests/stress_test.cpp)
target_include_directories(stress_test PRIVATE include)
target_link_libraries(stress_test PRIVATE Threads::Threads)
add_test(NAME test_stress COMMAND stress_test)

add_executable(benchmark_runner benchmark/benchmark.cpp)
target_include_directories(benchmark_runner PRIVATE include)
target_link_libraries(benchmark_runner PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(correctness PRIVATE /W4 /UNDEBUG)
    target_compile_options(stress_test PRIVATE /W4 /UNDEBUG)
    target_compile_options(benchmark_runner PRIVATE /W4)
else()
    target_compile_options(correctness PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_compile_options(stress_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_compile_options(benchmark_runner PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(RESULTS_DIR "${CMAKE_BINARY_DIR}/results")
file(MAKE_DIRECTORY "${RESULTS_DIR}")

# Save CPU information with the experiment results.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(LSCPU_EXECUTABLE lscpu)
    if(LSCPU_EXECUTABLE)
        execute_process(
            COMMAND "${LSCPU_EXECUTABLE}"
            OUTPUT_FILE "${RESULTS_DIR}/lscpu.txt"
            ERROR_QUIET
        )
    endif()
endif()

# Full scheduler-controlled benchmark.
# On Linux, perf wraps the benchmark so the same run produces both CSVs and
# hardware/OS counters. If perf is unavailable, the benchmark still runs.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(PERF_EXECUTABLE perf)
endif()

if(PERF_EXECUTABLE)
    add_test(
        NAME test_full_scheduler
        COMMAND "${PERF_EXECUTABLE}" stat
                -o "${RESULTS_DIR}/perf_scheduler.txt"
                -e context-switches
                -e cpu-migrations
                -e cycles
                -e instructions
                -e cache-references
                -e cache-misses
                --
                $<TARGET_FILE:benchmark_runner>
                --items ${EXPERIMENT_ITEMS}
                --runs ${EXPERIMENT_RUNS}
                --output "${RESULTS_DIR}/throughput_scheduler.csv"
                --latency-output "${RESULTS_DIR}/latency_scheduler.csv"
    )
else()
    add_test(
        NAME test_full_scheduler
        COMMAND $<TARGET_FILE:benchmark_runner>
                --items ${EXPERIMENT_ITEMS}
                --runs ${EXPERIMENT_RUNS}
                --output "${RESULTS_DIR}/throughput_scheduler.csv"
                --latency-output "${RESULTS_DIR}/latency_scheduler.csv"
    )
endif()

set_tests_properties(test_full_scheduler PROPERTIES
    RUN_SERIAL TRUE
    DEPENDS "test_correctness;test_stress"
)

# Full affinity-pinned benchmark. The queue code is unchanged; only benchmark
# worker placement differs. This experiment is Linux-only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(PERF_EXECUTABLE)
        add_test(
            NAME test_full_pinned
            COMMAND "${PERF_EXECUTABLE}" stat
                    -o "${RESULTS_DIR}/perf_pinned.txt"
                    -e context-switches
                    -e cpu-migrations
                    -e cycles
                    -e instructions
                    -e cache-references
                    -e cache-misses
                    --
                    $<TARGET_FILE:benchmark_runner>
                    --items ${EXPERIMENT_ITEMS}
                    --runs ${EXPERIMENT_RUNS}
                    --pin
                    --output "${RESULTS_DIR}/throughput_pinned.csv"
                    --latency-output "${RESULTS_DIR}/latency_pinned.csv"
        )
    else()
        add_test(
            NAME test_full_pinned
            COMMAND $<TARGET_FILE:benchmark_runner>
                    --items ${EXPERIMENT_ITEMS}
                    --runs ${EXPERIMENT_RUNS}
                    --pin
                    --output "${RESULTS_DIR}/throughput_pinned.csv"
                    --latency-output "${RESULTS_DIR}/latency_pinned.csv"
        )
    endif()

    set_tests_properties(test_full_pinned PROPERTIES
        RUN_SERIAL TRUE
        DEPENDS test_full_scheduler
    )
endif()
