cmake_minimum_required(VERSION 3.20)
project(mc_runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(MC_RUNTIME_SOURCES
    src/aggregation/aggregate.cpp
    src/codec.cpp
    src/failure_injection.cpp
    src/hash.cpp
    src/identity.cpp
    src/metrics.cpp
    src/models/gbm.cpp
    src/parse.cpp
    src/persistence_codec.cpp
    src/rng/philox_rng.cpp
    src/run_store.cpp
    src/runtime/coordinator.cpp
    src/runtime/engine.cpp
    src/run_spec.cpp
)
file(GLOB MC_RUNTIME_HEADERS CONFIGURE_DEPENDS include/mc/*.hpp)
file(GLOB_RECURSE MC_RUNTIME_INTERNAL_HEADERS CONFIGURE_DEPENDS src/*.hpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${MC_RUNTIME_SOURCES} ${MC_RUNTIME_HEADERS}
             ${MC_RUNTIME_INTERNAL_HEADERS})
set(MC_SOURCE_HASH_MATERIAL "")
foreach(MC_SOURCE_PATH IN LISTS MC_RUNTIME_SOURCES MC_RUNTIME_HEADERS
                                MC_RUNTIME_INTERNAL_HEADERS)
    if(IS_ABSOLUTE "${MC_SOURCE_PATH}")
        set(MC_SOURCE_FILE "${MC_SOURCE_PATH}")
        file(RELATIVE_PATH MC_SOURCE_LABEL
             "${CMAKE_CURRENT_SOURCE_DIR}" "${MC_SOURCE_PATH}")
    else()
        set(MC_SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${MC_SOURCE_PATH}")
        set(MC_SOURCE_LABEL "${MC_SOURCE_PATH}")
    endif()
    file(SHA256 "${MC_SOURCE_FILE}" MC_FILE_HASH)
    string(APPEND MC_SOURCE_HASH_MATERIAL
           "${MC_SOURCE_LABEL}:${MC_FILE_HASH};")
endforeach()
string(SHA256 MC_SOURCE_REVISION "${MC_SOURCE_HASH_MATERIAL}")
string(TOUPPER "${CMAKE_BUILD_TYPE}" MC_BUILD_TYPE_UPPER)
set(MC_EFFECTIVE_FLAGS
    "${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_${MC_BUILD_TYPE_UPPER}};"
    "${CMAKE_CXX_FLAGS_DEBUG};${CMAKE_CXX_FLAGS_RELEASE};"
    "${CMAKE_CXX_FLAGS_RELWITHDEBINFO};${CMAKE_CXX_FLAGS_MINSIZEREL};"
    "${CMAKE_EXE_LINKER_FLAGS};${CMAKE_INTERPROCEDURAL_OPTIMIZATION};"
    "-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow;-ffp-contract=off")
string(SHA256 MC_BUILD_FLAGS_ID "${MC_EFFECTIVE_FLAGS}")

add_library(mc_runtime ${MC_RUNTIME_SOURCES})
target_include_directories(mc_runtime PUBLIC include)
target_link_libraries(mc_runtime PUBLIC Threads::Threads)
target_compile_options(mc_runtime PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=off
)
target_compile_definitions(mc_runtime PRIVATE
    MC_FP_CONTRACT_MODE="off"
    MC_SOURCE_REVISION="${MC_SOURCE_REVISION}"
    MC_BUILD_FLAGS_ID="${MC_BUILD_FLAGS_ID}"
    MC_BUILD_CONFIG="$<CONFIG>"
    MC_CPU_POLICY="compiler-default"
)

add_executable(run_simulation tools/run_simulation.cpp)
target_link_libraries(run_simulation PRIVATE mc_runtime)

add_executable(benchmark_scaling tools/benchmark_scaling.cpp)
target_link_libraries(benchmark_scaling PRIVATE mc_runtime)

add_executable(benchmark_aggregation tools/benchmark_aggregation.cpp)
target_link_libraries(benchmark_aggregation PRIVATE mc_runtime)

add_executable(benchmark_persistence tools/benchmark_persistence.cpp)
target_link_libraries(benchmark_persistence PRIVATE mc_runtime)

add_executable(replay_failure tools/replay_failure.cpp)
target_link_libraries(replay_failure PRIVATE mc_runtime)

add_executable(run_crash_matrix tools/run_crash_matrix.cpp)
target_link_libraries(run_crash_matrix PRIVATE mc_runtime)

include(CTest)
if(BUILD_TESTING)
    add_executable(mc_tests tests/test_main.cpp)
    target_link_libraries(mc_tests PRIVATE mc_runtime)
    add_test(NAME mc_tests COMMAND mc_tests)
    add_test(NAME benchmark_scaling_smoke COMMAND benchmark_scaling
             --scenarios 4096 --repeats 1 --max-workers 2
             --queue-capacities 0,1)
    add_test(NAME benchmark_aggregation_smoke COMMAND benchmark_aggregation
             --scenarios 4096 --repeats 1 --max-workers 2
             --block-size 512)
    add_test(NAME benchmark_scaling_rejects_single_scenario
             COMMAND benchmark_scaling --scenarios 1 --repeats 1
             --max-workers 1)
    add_test(NAME benchmark_scaling_rejects_oversized_worker_count
             COMMAND benchmark_scaling --scenarios 2 --repeats 1
             --max-workers 257)
    add_test(NAME run_crash_matrix_help COMMAND run_crash_matrix --help)
    add_test(NAME replay_failure_help COMMAND replay_failure --help)
    # Launch by a bare name through PATH so the persistence tool's crash-child
    # self-exec path is covered, not only CTest's resolved target path.
    add_test(NAME benchmark_persistence_smoke
             COMMAND ${CMAKE_COMMAND} -E env
             "PATH=$<TARGET_FILE_DIR:benchmark_persistence>:$ENV{PATH}"
             benchmark_persistence --scenarios 1024 --workers 2 --repeats 1
             --block-sizes 256 --checkpoint-intervals 1,4)
    set_tests_properties(benchmark_scaling_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "scenarios,time_steps,repeats,block_size,queue_capacity_requested")
    set_tests_properties(benchmark_aggregation_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "scenarios,time_steps,repeats,strategy,block_size,workers_requested")
    set_tests_properties(
        benchmark_scaling_rejects_single_scenario
        benchmark_scaling_rejects_oversized_worker_count
        PROPERTIES
            WILL_FAIL TRUE
            FAIL_REGULAR_EXPRESSION
                "scenarios,time_steps,repeats,block_size,queue_capacity_requested")
    set_tests_properties(run_crash_matrix_help PROPERTIES
        PASS_REGULAR_EXPRESSION "Usage: run_crash_matrix")
    set_tests_properties(replay_failure_help PROPERTIES
        PASS_REGULAR_EXPRESSION "Usage: replay_failure")
    set_tests_properties(benchmark_persistence_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "crash_recovery_open_ns,recovery_computed_scenarios,recomputed_scenarios_after_crash"
        TIMEOUT 60)
endif()
