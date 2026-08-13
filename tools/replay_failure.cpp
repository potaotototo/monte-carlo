#include "mc/failure_injection.hpp"
#include "mc/identity.hpp"
#include "mc/parse.hpp"
#include "mc/persistence.hpp"

#include "process_watchdog.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

void print_help(std::ostream& output) {
    output << "Usage: replay_failure DESCRIPTOR --run-dir EMPTY_PATH "
              "[--timeout-seconds N]\n";
}

int run_child(const std::filesystem::path& source_descriptor,
              const std::filesystem::path& run_directory,
              const std::filesystem::path& observed_descriptor) {
    try {
        const mc::ReplayDescriptor replay =
            mc::read_replay_descriptor(source_descriptor);
        mc::RunStoreConfig store_config;
        store_config.run_directory = run_directory;
        store_config.checkpoint_interval_blocks =
            replay.checkpoint_interval_blocks;
        store_config.max_storage_bytes = replay.max_storage_bytes;
        store_config.max_storage_files = replay.max_storage_files;
        store_config.min_free_space_bytes = replay.min_free_space_bytes;
        store_config.max_manifest_bytes = replay.max_manifest_bytes;
        mc::FailureInjectionConfig injection = replay.injection;
        injection.replay_descriptor_path = observed_descriptor;
        store_config.failure_injection = injection;
        static_cast<void>(mc::run_parallel_durable(
            replay.spec, replay.engine_config, store_config));
        return 95;
    } catch (const std::exception& error) {
        std::cerr << "replay child failed before injection: "
                  << error.what() << '\n';
        return 94;
    }
}

void require_empty_target(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            throw std::runtime_error("cannot inspect replay run directory: " +
                                     error.message());
        }
        return;
    }
    if (!std::filesystem::is_directory(path, error) || error) {
        throw std::runtime_error("replay run target must be a directory");
    }
    if (std::filesystem::directory_iterator(path, error) !=
            std::filesystem::directory_iterator{} ||
        error) {
        throw std::runtime_error(
            "replay run target must be empty to preserve existing data");
    }
}

bool same_replay(const mc::ReplayDescriptor& left,
                 const mc::ReplayDescriptor& right) {
    return left.version == right.version &&
           left.run_spec_hash == right.run_spec_hash &&
           left.build_fingerprint == right.build_fingerprint &&
           left.engine_config.worker_count == right.engine_config.worker_count &&
           left.engine_config.block_size == right.engine_config.block_size &&
           left.engine_config.assignment_queue_capacity ==
               right.engine_config.assignment_queue_capacity &&
           left.engine_config.completion_queue_capacity ==
               right.engine_config.completion_queue_capacity &&
           left.engine_config.max_materialized_blocks ==
               right.engine_config.max_materialized_blocks &&
           left.checkpoint_interval_blocks ==
               right.checkpoint_interval_blocks &&
           left.max_storage_bytes == right.max_storage_bytes &&
           left.max_storage_files == right.max_storage_files &&
           left.min_free_space_bytes == right.min_free_space_bytes &&
           left.max_manifest_bytes == right.max_manifest_bytes &&
           left.injection.failure_seed == right.injection.failure_seed &&
           left.injection.deterministic_scheduler_seed ==
               right.injection.deterministic_scheduler_seed &&
           left.injection.selected_point == right.injection.selected_point &&
           left.injection.selected_occurrence ==
               right.injection.selected_occurrence &&
           left.observed_trace_hash == right.observed_trace_hash &&
           left.observed_trace_events == right.observed_trace_events &&
           left.run_incarnation == right.run_incarnation &&
           left.block_id == right.block_id &&
           left.checkpoint_sequence == right.checkpoint_sequence;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--child") {
            return run_child(argv[2], argv[3], argv[4]);
        }
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            print_help(std::cout);
            return 0;
        }
        if (argc < 4) {
            print_help(std::cerr);
            return 2;
        }
        const std::filesystem::path executable =
            mc::tool::resolve_executable_path(argv[0]);
        const std::filesystem::path descriptor_path = argv[1];
        std::filesystem::path run_directory;
        std::uint64_t timeout_seconds = 300U;
        for (int index = 2; index < argc; ++index) {
            if (index + 1 >= argc) {
                throw std::invalid_argument("missing replay option value");
            }
            const std::string_view option = argv[index];
            const std::string_view value = argv[++index];
            if (option == "--run-dir") {
                run_directory = value;
            } else if (option == "--timeout-seconds") {
                timeout_seconds = mc::parse_u64(value, "timeout_seconds");
            } else {
                throw std::invalid_argument("unknown replay option: " +
                                            std::string(option));
            }
        }
        if (run_directory.empty()) {
            throw std::invalid_argument("--run-dir is required");
        }
        const std::chrono::seconds timeout =
            mc::tool::checked_watchdog_timeout(timeout_seconds);
        require_empty_target(run_directory);
        const mc::ReplayDescriptor expected =
            mc::read_replay_descriptor(descriptor_path);
        if (expected.build_fingerprint != mc::current_build_identity().hash) {
            throw std::runtime_error(
                "replay descriptor was produced by an incompatible build");
        }
        const std::filesystem::path observed_descriptor =
            run_directory.parent_path() /
            (run_directory.filename().string() + ".observed.replay");
        if (std::filesystem::exists(observed_descriptor)) {
            throw std::runtime_error(
                "observed replay path already exists: " +
                observed_descriptor.string());
        }

        const pid_t child = ::fork();
        if (child < 0) {
            throw std::runtime_error("cannot fork replay child");
        }
        if (child == 0) {
            const std::string executable_text = executable.string();
            const std::string descriptor_text = descriptor_path.string();
            const std::string run_text = run_directory.string();
            const std::string observed_text = observed_descriptor.string();
            ::execl(executable_text.c_str(), executable_text.c_str(), "--child",
                    descriptor_text.c_str(), run_text.c_str(),
                    observed_text.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        const int exit_code =
            mc::tool::wait_for_child(child, timeout, "replay");
        if (exit_code != mc::kFailureInjectionExitCode) {
            throw std::runtime_error(
                "replay child did not reach the selected failure point; exit=" +
                std::to_string(exit_code));
        }
        const mc::ReplayDescriptor observed =
            mc::read_replay_descriptor(observed_descriptor);
        if (!same_replay(observed, expected)) {
            throw std::runtime_error(
                "crash point was reached, but the complete replay identity did "
                "not reproduce");
        }
        std::cout << "reproduced "
                  << mc::failure_point_name(expected.injection.selected_point)
                  << " occurrence="
                  << expected.injection.selected_occurrence
                  << " trace=" << mc::to_hex(expected.observed_trace_hash)
                  << "\nrun store: " << run_directory
                  << "\nobserved descriptor: " << observed_descriptor << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
