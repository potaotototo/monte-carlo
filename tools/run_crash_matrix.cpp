#include "mc/failure_injection.hpp"
#include "mc/codec.hpp"
#include "mc/identity.hpp"
#include "mc/parse.hpp"
#include "mc/persistence.hpp"

#include "process_watchdog.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

struct MatrixCase {
    mc::RunSpec spec;
    mc::EngineConfig engine_config;
    mc::RunStoreConfig store_config;
};

void print_help(std::ostream& output) {
    output << "Usage: run_crash_matrix --workspace EMPTY_PATH "
              "[--iterations N] [--first-seed N] "
              "[--timeout-seconds N]\n";
}

std::uint64_t mix_seed(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

std::string padded_case(std::uint64_t index) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "case_" << std::setw(8) << std::setfill('0') << index;
    return stream.str();
}

MatrixCase make_case(std::uint64_t failure_seed,
                     const std::filesystem::path& case_directory) {
    const std::uint64_t topology =
        mix_seed(failure_seed ^ 0xA0761D6478BD642FULL);
    constexpr std::array<std::uint64_t, 6> block_sizes = {
        16U, 24U, 32U, 48U, 64U, 96U};
    constexpr std::array<std::size_t, 4> queue_capacities = {0U, 1U, 2U, 4U};

    MatrixCase test_case;
    test_case.spec.global_seed =
        0xD1B54A32D192ED03ULL ^ failure_seed;
    test_case.spec.num_time_steps =
        static_cast<std::uint32_t>(1U + ((topology >> 8U) % 4U));
    test_case.spec.payoff_type =
        (topology & 1U) == 0U ? mc::PayoffType::EuropeanCall
                              : mc::PayoffType::AsianCall;
    test_case.spec.antithetic = (topology & 2U) != 0U;
    test_case.engine_config.worker_count = 1U;
    test_case.engine_config.block_size = block_sizes[static_cast<std::size_t>(
        (topology >> 12U) % block_sizes.size())];
    const std::uint64_t block_count = 3U + ((topology >> 20U) % 8U);
    const std::uint64_t tail_units = test_case.spec.antithetic
                                         ? test_case.engine_config.block_size / 2U
                                         : test_case.engine_config.block_size;
    const std::uint64_t tail_multiplier =
        test_case.spec.antithetic ? 2U : 1U;
    const std::uint64_t tail = tail_multiplier *
        (1U + ((topology >> 28U) % tail_units));
    test_case.spec.total_scenarios =
        (block_count - 1U) * test_case.engine_config.block_size + tail;
    test_case.engine_config.assignment_queue_capacity =
        queue_capacities[static_cast<std::size_t>(
            (topology >> 36U) % queue_capacities.size())];
    test_case.engine_config.completion_queue_capacity =
        queue_capacities[static_cast<std::size_t>(
            (topology >> 44U) % queue_capacities.size())];
    test_case.store_config.run_directory = case_directory / "run";
    test_case.store_config.checkpoint_interval_blocks =
        1U + ((topology >> 52U) % std::min<std::uint64_t>(4U, block_count - 1U));
    test_case.store_config.min_free_space_bytes = 0U;
    test_case.store_config.failure_injection =
        mc::failure_injection_from_seed(
            failure_seed, case_directory / "failure.replay");
    return test_case;
}

bool aggregates_equal(const mc::AggregateStats& left,
                      const mc::AggregateStats& right) {
    return left.n == right.n &&
           std::bit_cast<std::uint64_t>(left.mean) ==
               std::bit_cast<std::uint64_t>(right.mean) &&
           std::bit_cast<std::uint64_t>(left.m2) ==
               std::bit_cast<std::uint64_t>(right.m2) &&
           std::bit_cast<std::uint64_t>(left.min) ==
               std::bit_cast<std::uint64_t>(right.min) &&
           std::bit_cast<std::uint64_t>(left.max) ==
               std::bit_cast<std::uint64_t>(right.max);
}

bool descriptor_matches_case(const mc::ReplayDescriptor& replay,
                             const MatrixCase& test_case,
                             std::uint64_t failure_seed) {
    const mc::FailureInjectionConfig& expected =
        *test_case.store_config.failure_injection;
    return replay.run_spec_hash == mc::run_spec_hash(test_case.spec) &&
           replay.build_fingerprint == mc::current_build_identity().hash &&
           replay.engine_config.worker_count ==
               test_case.engine_config.worker_count &&
           replay.engine_config.block_size == test_case.engine_config.block_size &&
           replay.engine_config.assignment_queue_capacity ==
               test_case.engine_config.assignment_queue_capacity &&
           replay.engine_config.completion_queue_capacity ==
               test_case.engine_config.completion_queue_capacity &&
           replay.engine_config.max_materialized_blocks ==
               test_case.engine_config.max_materialized_blocks &&
           replay.checkpoint_interval_blocks ==
               test_case.store_config.checkpoint_interval_blocks &&
           replay.max_storage_bytes ==
               test_case.store_config.max_storage_bytes &&
           replay.max_storage_files ==
               test_case.store_config.max_storage_files &&
           replay.min_free_space_bytes ==
               test_case.store_config.min_free_space_bytes &&
           replay.max_manifest_bytes ==
               test_case.store_config.max_manifest_bytes &&
           replay.injection.failure_seed == failure_seed &&
           replay.injection.deterministic_scheduler_seed == 0U &&
           replay.injection.selected_point == expected.selected_point &&
           replay.injection.selected_occurrence == expected.selected_occurrence;
}

int run_child(std::uint64_t failure_seed,
              const std::filesystem::path& case_directory) {
    try {
        const MatrixCase test_case = make_case(failure_seed, case_directory);
        static_cast<void>(mc::run_parallel_durable(
            test_case.spec, test_case.engine_config,
            test_case.store_config));
        return 95;
    } catch (const std::exception& error) {
        std::cerr << "matrix child failed before injection: "
                  << error.what() << '\n';
        return 94;
    }
}

int run_recovery_child(std::uint64_t failure_seed,
                       const std::filesystem::path& case_directory) {
    try {
        const MatrixCase test_case = make_case(failure_seed, case_directory);
        const mc::RunResult clean =
            mc::run_parallel(test_case.spec, test_case.engine_config);
        mc::RunStoreConfig recovery_config = test_case.store_config;
        recovery_config.failure_injection.reset();
        const mc::DurableRunResult recovered = mc::run_parallel_durable(
            test_case.spec, test_case.engine_config, recovery_config);
        const mc::DurableRunResult reopened = mc::run_parallel_durable(
            test_case.spec, test_case.engine_config, recovery_config);
        if (!aggregates_equal(recovered.run_result.aggregate,
                              clean.aggregate) ||
            !aggregates_equal(reopened.run_result.aggregate,
                              clean.aggregate) ||
            recovered.recovered_blocks + recovered.computed_blocks !=
                clean.block_count ||
            reopened.recovered_blocks != clean.block_count ||
            reopened.computed_blocks != 0U) {
            throw std::runtime_error("recovery invariant failed");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "matrix recovery child failed: " << error.what() << '\n';
        return 93;
    }
}

int run_supervised_phase(const std::filesystem::path& executable,
                         std::string_view mode,
                         std::uint64_t failure_seed,
                         const std::filesystem::path& case_directory,
                         std::chrono::seconds timeout) {
    const pid_t child = ::fork();
    if (child < 0) {
        throw std::runtime_error("cannot fork crash-matrix child");
    }
    if (child == 0) {
        const std::string executable_text = executable.string();
        const std::string mode_text(mode);
        const std::string seed_text = std::to_string(failure_seed);
        const std::string case_text = case_directory.string();
        ::execl(executable_text.c_str(), executable_text.c_str(),
                mode_text.c_str(), seed_text.c_str(), case_text.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return mc::tool::wait_for_child(
        child, timeout,
        std::string(mode == "--child" ? "injection" : "recovery") +
            " for " + case_directory.string());
}

void require_empty_workspace(const std::filesystem::path& workspace) {
    std::error_code error;
    std::filesystem::create_directories(workspace, error);
    if (error) {
        throw std::runtime_error("cannot create matrix workspace: " +
                                 error.message());
    }
    if (std::filesystem::directory_iterator(workspace, error) !=
            std::filesystem::directory_iterator{} ||
        error) {
        throw std::runtime_error(
            "matrix workspace must be empty; successful cases are removed");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--child") {
            return run_child(mc::parse_u64(argv[2], "failure_seed"), argv[3]);
        }
        if (argc == 4 && std::string_view(argv[1]) == "--recovery-child") {
            return run_recovery_child(
                mc::parse_u64(argv[2], "failure_seed"), argv[3]);
        }

        std::uint64_t iterations = 100U;
        std::uint64_t first_seed = 1U;
        std::uint64_t timeout_seconds = 30U;
        std::filesystem::path workspace;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--help") {
                print_help(std::cout);
                return 0;
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument("missing crash-matrix option value");
            }
            const std::string_view value = argv[++index];
            if (argument == "--iterations") {
                iterations = mc::parse_u64(value, "iterations");
            } else if (argument == "--first-seed") {
                first_seed = mc::parse_u64(value, "first_seed");
            } else if (argument == "--workspace") {
                workspace = value;
            } else if (argument == "--timeout-seconds") {
                timeout_seconds = mc::parse_u64(value, "timeout_seconds");
            } else {
                throw std::invalid_argument("unknown option: " +
                                            std::string(argument));
            }
        }
        if (iterations == 0U || workspace.empty()) {
            print_help(std::cerr);
            return 2;
        }
        if (first_seed >
            std::numeric_limits<std::uint64_t>::max() - (iterations - 1U)) {
            throw std::overflow_error("crash-matrix seed range overflows");
        }
        const std::chrono::seconds timeout =
            mc::tool::checked_watchdog_timeout(timeout_seconds);
        const std::filesystem::path executable =
            mc::tool::resolve_executable_path(argv[0]);
        require_empty_workspace(workspace);
        std::array<bool, mc::kFailurePoints.size()> covered{};
        std::set<std::uint64_t> covered_block_sizes;
        std::set<std::uint64_t> covered_block_counts;
        std::set<std::uint64_t> covered_checkpoint_intervals;
        std::set<std::size_t> covered_assignment_capacities;
        std::set<std::size_t> covered_completion_capacities;
        bool covered_partial_final_block = false;

        for (std::uint64_t index = 0; index < iterations; ++index) {
            const std::uint64_t failure_seed = first_seed + index;
            const std::filesystem::path case_directory =
                workspace / padded_case(index);
            if (!std::filesystem::create_directory(case_directory)) {
                throw std::runtime_error("matrix case directory already exists");
            }
            const MatrixCase test_case = make_case(failure_seed, case_directory);
            const std::uint64_t block_count =
                1U + (test_case.spec.total_scenarios - 1U) /
                         test_case.engine_config.block_size;
            covered_block_sizes.insert(test_case.engine_config.block_size);
            covered_block_counts.insert(block_count);
            covered_checkpoint_intervals.insert(
                test_case.store_config.checkpoint_interval_blocks);
            covered_assignment_capacities.insert(
                test_case.engine_config.assignment_queue_capacity);
            covered_completion_capacities.insert(
                test_case.engine_config.completion_queue_capacity);
            covered_partial_final_block =
                covered_partial_final_block ||
                test_case.spec.total_scenarios %
                        test_case.engine_config.block_size !=
                    0U;

            const int child_exit = run_supervised_phase(
                executable, "--child", failure_seed, case_directory, timeout);
            if (child_exit != mc::kFailureInjectionExitCode) {
                throw std::runtime_error(
                    "child missed selected injection; replay retained at " +
                    case_directory.string());
            }
            const mc::ReplayDescriptor replay = mc::read_replay_descriptor(
                case_directory / "failure.replay");
            if (!descriptor_matches_case(replay, test_case, failure_seed)) {
                throw std::runtime_error(
                    "crash descriptor does not match its seeded matrix case; "
                    "replay retained at " + case_directory.string());
            }
            for (std::size_t point_index = 0;
                 point_index < mc::kFailurePoints.size(); ++point_index) {
                if (replay.injection.selected_point ==
                    mc::kFailurePoints[point_index]) {
                    covered[point_index] = true;
                }
            }

            const int recovery_exit = run_supervised_phase(
                executable, "--recovery-child", failure_seed,
                case_directory, timeout);
            if (recovery_exit != 0) {
                throw std::runtime_error(
                    "recovery phase failed; replay retained at " +
                    (case_directory / "failure.replay").string());
            }

            std::error_code cleanup_error;
            static_cast<void>(
                std::filesystem::remove_all(case_directory, cleanup_error));
            if (cleanup_error) {
                throw std::runtime_error("cannot remove successful matrix case: " +
                                         cleanup_error.message());
            }
            if ((index + 1U) % 100U == 0U || index + 1U == iterations) {
                std::cout << "validated " << index + 1U << '/' << iterations
                          << " seeded crash schedules\n";
            }
        }

        std::size_t coverage = 0;
        for (const bool hit : covered) {
            coverage += hit ? 1U : 0U;
        }
        std::cout << "crash schedules validated; failure-point coverage "
                  << coverage << '/' << mc::kFailurePoints.size() << '\n';
        std::cout << "topology coverage: block_sizes="
                  << covered_block_sizes.size()
                  << " block_counts=" << covered_block_counts.size()
                  << " checkpoint_intervals="
                  << covered_checkpoint_intervals.size()
                  << " assignment_capacities="
                  << covered_assignment_capacities.size()
                  << " completion_capacities="
                  << covered_completion_capacities.size()
                  << " partial_final_block="
                  << (covered_partial_final_block ? "yes" : "no") << '\n';
        const bool topology_coverage_ok =
            iterations < 32U ||
            (covered_block_sizes.size() >= 4U &&
             covered_block_counts.size() >= 4U &&
             covered_checkpoint_intervals.size() >= 2U &&
             covered_assignment_capacities.size() >= 2U &&
             covered_completion_capacities.size() >= 2U &&
             covered_partial_final_block);
        if (coverage != mc::kFailurePoints.size()) {
            std::cerr << "crash matrix did not cover every failure point\n";
            return 3;
        }
        if (!topology_coverage_ok) {
            std::cerr << "crash matrix did not meet its topology-diversity gate\n";
            return 4;
        }
        std::cout << "crash matrix passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
