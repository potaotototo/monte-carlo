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

add_executable(replay_failure tools/replay_failure.cpp)
target_link_libraries(replay_failure PRIVATE mc_runtime)

add_executable(run_crash_matrix tools/run_crash_matrix.cpp)
target_link_libraries(run_crash_matrix PRIVATE mc_runtime)

include(CTest)
if(BUILD_TESTING)
    add_executable(mc_tests tests/test_main.cpp)
    target_link_libraries(mc_tests PRIVATE mc_runtime)
    add_test(NAME mc_tests COMMAND mc_tests)
endif()
