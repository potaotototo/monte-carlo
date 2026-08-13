#include "mc/engine.hpp"
#include "mc/failure_injection.hpp"
#include "mc/parse.hpp"
#include "mc/persistence.hpp"
#include "mc/run_spec.hpp"

#include "process_watchdog.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class BenchmarkWorkspace {
public:
    explicit BenchmarkWorkspace(const std::filesystem::path& parent) {
        static std::atomic<std::uint64_t> sequence{0U};
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error || !std::filesystem::is_directory(parent)) {
            throw std::runtime_error("cannot create benchmark workspace parent");
        }
        for (std::uint32_t attempt = 0U; attempt < 100U; ++attempt) {
            root_ = parent /
                    ("mc-r4-persistence-" +
                     std::to_string(static_cast<std::uint64_t>(::getpid())) +
                     "-" + std::to_string(sequence.fetch_add(1U)) + "-" +
                     std::to_string(attempt));
            if (std::filesystem::create_directory(root_, error)) {
                return;
            }
            error.clear();
        }
        throw std::runtime_error("cannot create unique benchmark workspace");
    }

    BenchmarkWorkspace(const BenchmarkWorkspace&) = delete;
    BenchmarkWorkspace& operator=(const BenchmarkWorkspace&) = delete;

    ~BenchmarkWorkspace() {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(root_, ignored));
    }

    [[nodiscard]] std::filesystem::path run_directory(
        std::uint64_t block_size,
        std::uint64_t interval,
        std::size_t repeat) const {
        return root_ /
               ("block-" + std::to_string(block_size) + "-checkpoint-" +
                std::to_string(interval) + "-repeat-" +
                std::to_string(repeat));
    }

    [[nodiscard]] std::filesystem::path crash_run_directory(
        std::uint64_t block_size,
        std::uint64_t interval,
        std::size_t repeat) const {
        return root_ /
               ("crash-block-" + std::to_string(block_size) +
                "-checkpoint-" + std::to_string(interval) + "-repeat-" +
                std::to_string(repeat));
    }

    [[nodiscard]] std::filesystem::path replay_descriptor_path(
        std::uint64_t block_size,
        std::uint64_t interval,
        std::size_t repeat) const {
        return root_ /
               ("crash-block-" + std::to_string(block_size) +
                "-checkpoint-" + std::to_string(interval) + "-repeat-" +
                std::to_string(repeat) + ".replay");
    }

private:
    std::filesystem::path root_;
};

std::string require_value(int argc, char** argv, int& index) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") +
                                    argv[index]);
    }
    ++index;
    return argv[index];
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2U;
    if (values.size() % 2U == 0U) {
        return 0.5 * (values[middle - 1U] + values[middle]);
    }
    return values[middle];
}

std::vector<std::uint64_t> parse_u64_list(std::string_view text,
                                          std::string_view field,
                                          bool allow_zero) {
    std::vector<std::uint64_t> values;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view token = text.substr(0U, comma);
        if (token.empty()) {
            throw std::invalid_argument(std::string(field) +
                                        " contains an empty value");
        }
        const std::uint64_t value = mc::parse_u64(token, field);
        if (!allow_zero && value == 0U) {
            throw std::invalid_argument(std::string(field) +
                                        " values must be positive");
        }
        if (std::find(values.begin(), values.end(), value) == values.end()) {
            values.push_back(value);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1U);
    }
    if (values.empty()) {
        throw std::invalid_argument(std::string(field) + " must not be empty");
    }
    return values;
}

double required_metric(const std::optional<std::uint64_t>& value,
                       std::string_view name) {
    if (!value.has_value()) {
        throw std::runtime_error("benchmark produced no samples for " +
                                 std::string(name));
    }
    return static_cast<double>(*value);
}

double process_cpu_seconds() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        throw std::runtime_error("getrusage failed while benchmarking");
    }
    return static_cast<double>(usage.ru_utime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec) / 1.0e6 +
           static_cast<double>(usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_stime.tv_usec) / 1.0e6;
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

struct CrashRecoveryMeasurement {
    double seconds = 0.0;
    double open_ns = 0.0;
    std::uint64_t recovery_computed_scenarios = 0U;
    std::uint64_t recomputed_scenarios = 0U;
};

int run_crash_child(int argc, char** argv) {
    if (argc != 9) {
        return 93;
    }
    try {
        mc::RunSpec spec;
        spec.total_scenarios = mc::parse_u64(argv[4], "scenarios");
        const std::uint64_t steps = mc::parse_u64(argv[5], "steps");
        if (steps > mc::kMaxTimeSteps) {
            throw std::invalid_argument("steps exceeds RNG layout v1");
        }
        spec.num_time_steps = static_cast<std::uint32_t>(steps);

        mc::EngineConfig engine_config;
        engine_config.worker_count = 1U;
        engine_config.block_size = mc::parse_u64(argv[6], "block-size");

        mc::FailureInjectionConfig injection;
        injection.selected_point = mc::FailurePoint::ResultAfterRename;
        injection.selected_occurrence =
            mc::parse_u64(argv[8], "failure-occurrence");
        injection.replay_descriptor_path = argv[3];

        mc::RunStoreConfig store_config;
        store_config.run_directory = argv[2];
        store_config.checkpoint_interval_blocks =
            mc::parse_u64(argv[7], "checkpoint-interval");
        store_config.min_free_space_bytes = 0U;
        store_config.failure_injection = std::move(injection);
        static_cast<void>(mc::run_parallel_durable(
            spec, engine_config, store_config));
        return 95;
    } catch (const std::exception& error) {
        std::cerr << "crash benchmark child failed before injection: "
                  << error.what() << '\n';
        return 94;
    }
}

CrashRecoveryMeasurement measure_crash_recovery(
    const std::filesystem::path& executable,
    const mc::RunSpec& spec,
    std::uint64_t block_size,
    std::uint64_t checkpoint_interval,
    const std::filesystem::path& run_directory,
    const std::filesystem::path& replay_descriptor,
    const mc::AggregateStats& expected) {
    const std::uint64_t block_count =
        1U + (spec.total_scenarios - 1U) / block_size;
    const std::uint64_t second_checkpoint_occurrence =
        checkpoint_interval >
                std::numeric_limits<std::uint64_t>::max() -
                    checkpoint_interval
            ? std::numeric_limits<std::uint64_t>::max()
            : checkpoint_interval + checkpoint_interval;
    const std::uint64_t occurrence =
        std::min(second_checkpoint_occurrence, block_count);
    const pid_t child = ::fork();
    if (child < 0) {
        throw std::runtime_error("cannot fork persistence benchmark child");
    }
    if (child == 0) {
        const std::string executable_text = executable.string();
        const std::string run_text = run_directory.string();
        const std::string replay_text = replay_descriptor.string();
        const std::string scenarios_text =
            std::to_string(spec.total_scenarios);
        const std::string steps_text = std::to_string(spec.num_time_steps);
        const std::string block_text = std::to_string(block_size);
        const std::string interval_text =
            std::to_string(checkpoint_interval);
        const std::string occurrence_text = std::to_string(occurrence);
        ::execl(executable_text.c_str(), executable_text.c_str(),
                "--crash-child", run_text.c_str(), replay_text.c_str(),
                scenarios_text.c_str(), steps_text.c_str(),
                block_text.c_str(), interval_text.c_str(),
                occurrence_text.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    const int exit_code = mc::tool::wait_for_child(
        child, std::chrono::seconds{300}, "persistence benchmark crash");
    if (exit_code != mc::kFailureInjectionExitCode) {
        throw std::runtime_error(
            "persistence benchmark child did not reach the crash point; exit=" +
            std::to_string(exit_code));
    }
    const mc::ReplayDescriptor observed =
        mc::read_replay_descriptor(replay_descriptor);
    if (observed.injection.selected_point !=
            mc::FailurePoint::ResultAfterRename ||
        observed.injection.selected_occurrence != occurrence ||
        observed.block_id != occurrence - 1U) {
        throw std::runtime_error(
            "persistence crash did not reach the expected sequential block");
    }

    mc::EngineConfig engine_config;
    engine_config.worker_count = 1U;
    engine_config.block_size = block_size;
    mc::RunStoreConfig store_config;
    store_config.run_directory = run_directory;
    store_config.checkpoint_interval_blocks = checkpoint_interval;
    store_config.min_free_space_bytes = 0U;

    mc::RuntimeMetrics metrics;
    const auto started = std::chrono::steady_clock::now();
    const mc::DurableRunResult recovered = mc::run_parallel_durable(
        spec, engine_config, store_config, &metrics);
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const std::uint64_t committed_before_crash =
        occurrence > checkpoint_interval
            ? std::min(
                  spec.total_scenarios,
                  checkpoint_interval >
                          std::numeric_limits<std::uint64_t>::max() /
                              block_size
                      ? spec.total_scenarios
                      : checkpoint_interval * block_size)
            : 0U;
    const std::uint64_t computed_before_crash = std::min(
        spec.total_scenarios,
        occurrence > std::numeric_limits<std::uint64_t>::max() / block_size
            ? spec.total_scenarios
            : occurrence * block_size);
    const std::uint64_t recomputed =
        computed_before_crash - committed_before_crash;
    const std::uint64_t expected_recovery_compute =
        spec.total_scenarios - committed_before_crash;
    const bool aggregate_matches =
        aggregates_equal(recovered.run_result.aggregate, expected);
    if (!aggregate_matches ||
        recovered.computed_scenarios != expected_recovery_compute) {
        throw std::runtime_error(
            "crash recovery mismatch: aggregate_matches=" +
            std::string(aggregate_matches ? "true" : "false") +
            ", recomputed=" +
            std::to_string(recovered.computed_scenarios) +
            ", expected=" + std::to_string(expected_recovery_compute));
    }
    return {seconds, static_cast<double>(metrics.durable_open_ns),
            recovered.computed_scenarios, recomputed};
}

void print_help() {
    std::cout
        << "Usage: benchmark_persistence [options]\n"
        << "  --scenarios N       scenarios per run (default 200000)\n"
        << "  --steps N           GBM time steps (default 1)\n"
        << "  --workers N         worker count (default min(8, hardware))\n"
        << "  --repeats N         measured repetitions (default 3)\n"
        << "  --block-sizes LIST  comma-separated sizes (default 2048,10000)\n"
        << "  --checkpoint-intervals LIST  comma-separated positive cadences\n"
        << "  --workspace PATH    parent for temporary run stores\n"
        << "  --help              show this message\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "--crash-child") {
            return run_crash_child(argc, argv);
        }
        const std::filesystem::path executable =
            std::filesystem::absolute(argv[0]);
        mc::RunSpec spec;
        spec.total_scenarios = 200'000U;
        const std::size_t hardware =
            std::max<std::size_t>(1U, std::thread::hardware_concurrency());
        std::size_t workers = std::min<std::size_t>(8U, hardware);
        std::size_t repeats = 3U;
        std::vector<std::uint64_t> block_sizes{2'048U, 10'000U};
        std::vector<std::uint64_t> checkpoint_intervals{1U, 4U, 16U, 64U};
        std::filesystem::path workspace_parent =
            std::filesystem::temp_directory_path();

        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--help") {
                print_help();
                return 0;
            }
            if (argument == "--scenarios") {
                spec.total_scenarios = mc::parse_u64(
                    require_value(argc, argv, index), "scenarios");
            } else if (argument == "--steps") {
                const std::uint64_t value = mc::parse_u64(
                    require_value(argc, argv, index), "steps");
                if (value > mc::kMaxTimeSteps) {
                    throw std::invalid_argument("steps exceeds RNG layout v1");
                }
                spec.num_time_steps = static_cast<std::uint32_t>(value);
            } else if (argument == "--workers") {
                workers = mc::parse_size(
                    require_value(argc, argv, index), "workers");
            } else if (argument == "--repeats") {
                repeats = mc::parse_size(
                    require_value(argc, argv, index), "repeats");
            } else if (argument == "--block-sizes") {
                block_sizes = parse_u64_list(
                    require_value(argc, argv, index), "block-sizes", false);
            } else if (argument == "--checkpoint-intervals") {
                checkpoint_intervals = parse_u64_list(
                    require_value(argc, argv, index),
                    "checkpoint-intervals", false);
            } else if (argument == "--workspace") {
                workspace_parent = require_value(argc, argv, index);
            } else {
                throw std::invalid_argument("unknown argument: " +
                                            std::string(argument));
            }
        }
        if (workers == 0U || repeats == 0U) {
            throw std::invalid_argument("workers and repeats must be positive");
        }
        spec.validate();

        BenchmarkWorkspace workspace(workspace_parent);
        std::cout
            << "scenarios,time_steps,repeats,block_size,checkpoint_interval,workers,"
               "blocks,non_durable_median_seconds,"
               "durable_median_seconds,durable_scenarios_per_second,throughput_overhead_percent,"
               "cpu_utilization_percent,block_commit_p50_ns,block_commit_p95_ns,"
               "block_commit_p99_ns,result_persist_p50_ns,result_persist_p95_ns,"
               "result_persist_p99_ns,checkpoint_p50_ns,checkpoint_p95_ns,"
               "checkpoint_p99_ns,durable_open_ns,completed_restart_seconds,"
               "completed_restart_open_ns,crash_workers,crash_recovery_seconds,"
               "crash_recovery_open_ns,recovery_computed_scenarios,"
               "recomputed_scenarios_after_crash,max_recomputed_scenarios,"
               "result_files,manifest_files,bytes_written,checkpoint_samples_dropped,"
               "price\n";
        std::cout << std::fixed << std::setprecision(9);

        for (const std::uint64_t block_size : block_sizes) {
            mc::EngineConfig config;
            config.worker_count = workers;
            config.block_size = block_size;

            mc::RunSpec warmup = spec;
            warmup.total_scenarios =
                std::min<std::uint64_t>(spec.total_scenarios, 100'000U);
            static_cast<void>(mc::run_parallel(warmup, config));

            std::vector<double> non_durable_durations;
            non_durable_durations.reserve(repeats);
            mc::RunResult clean;
            for (std::size_t repeat = 0U; repeat < repeats; ++repeat) {
                const auto started = std::chrono::steady_clock::now();
                clean = mc::run_parallel(spec, config);
                non_durable_durations.push_back(std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started).count());
            }
            const double non_durable_seconds =
                median(std::move(non_durable_durations));
            const double non_durable_rate =
                static_cast<double>(spec.total_scenarios) /
                non_durable_seconds;

            for (const std::uint64_t interval : checkpoint_intervals) {
                std::vector<double> durations;
                std::vector<double> cpu_utilization;
                std::vector<double> commit_p50;
                std::vector<double> commit_p95;
                std::vector<double> commit_p99;
                std::vector<double> persist_p50;
                std::vector<double> persist_p95;
                std::vector<double> persist_p99;
                std::vector<double> checkpoint_p50;
                std::vector<double> checkpoint_p95;
                std::vector<double> checkpoint_p99;
                std::vector<double> durable_open;
                std::vector<double> restart_seconds;
                std::vector<double> restart_open;
                std::vector<double> crash_recovery_seconds;
                std::vector<double> crash_recovery_open;
                std::vector<double> bytes_written;
                mc::RuntimeMetrics last_metrics;
                std::uint64_t recovery_computed_scenarios = 0U;
                std::uint64_t recomputed_scenarios_after_crash = 0U;

                for (std::size_t repeat = 0U; repeat < repeats; ++repeat) {
                    mc::RunStoreConfig store_config;
                    store_config.run_directory = workspace.run_directory(
                        block_size, interval, repeat);
                    store_config.checkpoint_interval_blocks = interval;
                    store_config.min_free_space_bytes = 0U;

                    mc::RuntimeMetrics metrics;
                    const double cpu_started = process_cpu_seconds();
                    const auto started = std::chrono::steady_clock::now();
                    const mc::DurableRunResult durable =
                        mc::run_parallel_durable(
                            spec, config, store_config, &metrics);
                    const double seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - started).count();
                    if (!aggregates_equal(durable.run_result.aggregate,
                                          clean.aggregate)) {
                        throw std::runtime_error(
                            "durable benchmark changed the fixed-tree aggregate");
                    }
                    durations.push_back(seconds);
                    cpu_utilization.push_back(
                        100.0 * (process_cpu_seconds() - cpu_started) / seconds);
                    commit_p50.push_back(required_metric(
                        mc::latency_percentile_ns(metrics.block_commit_ns, 0.50),
                        "commit p50"));
                    commit_p95.push_back(required_metric(
                        mc::latency_percentile_ns(metrics.block_commit_ns, 0.95),
                        "commit p95"));
                    commit_p99.push_back(required_metric(
                        mc::latency_percentile_ns(metrics.block_commit_ns, 0.99),
                        "commit p99"));
                    persist_p50.push_back(required_metric(
                        mc::latency_percentile_ns(metrics.result_persist_ns, 0.50),
                        "persist p50"));
                    persist_p95.push_back(required_metric(
                        mc::latency_percentile_ns(metrics.result_persist_ns, 0.95),
                        "persist p95"));
                    persist_p99.push_back(required_metric(
                        mc::latency_percentile_ns(metrics.result_persist_ns, 0.99),
                        "persist p99"));
                    checkpoint_p50.push_back(required_metric(
                        mc::latency_percentile_ns(metrics.checkpoint_ns, 0.50),
                        "checkpoint p50"));
                    checkpoint_p95.push_back(required_metric(
                        mc::latency_percentile_ns(metrics.checkpoint_ns, 0.95),
                        "checkpoint p95"));
                    checkpoint_p99.push_back(required_metric(
                        mc::latency_percentile_ns(metrics.checkpoint_ns, 0.99),
                        "checkpoint p99"));
                    durable_open.push_back(
                        static_cast<double>(metrics.durable_open_ns));
                    bytes_written.push_back(
                        static_cast<double>(metrics.durable_io.bytes_written));
                    last_metrics = std::move(metrics);

                    mc::RuntimeMetrics recovered_metrics;
                    const auto restart_started = std::chrono::steady_clock::now();
                    const mc::DurableRunResult recovered =
                        mc::run_parallel_durable(
                            spec, config, store_config, &recovered_metrics);
                    restart_seconds.push_back(std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - restart_started).count());
                    restart_open.push_back(static_cast<double>(
                        recovered_metrics.durable_open_ns));
                    if (recovered.computed_blocks != 0U ||
                        !aggregates_equal(recovered.run_result.aggregate,
                                          clean.aggregate)) {
                        throw std::runtime_error(
                            "completed benchmark restart recomputed or changed output");
                    }

                    const CrashRecoveryMeasurement crash =
                        measure_crash_recovery(
                            executable, spec, block_size, interval,
                            workspace.crash_run_directory(
                                block_size, interval, repeat),
                            workspace.replay_descriptor_path(
                                block_size, interval, repeat),
                            clean.aggregate);
                    crash_recovery_seconds.push_back(crash.seconds);
                    crash_recovery_open.push_back(crash.open_ns);
                    if (repeat != 0U &&
                        (recovery_computed_scenarios !=
                             crash.recovery_computed_scenarios ||
                         recomputed_scenarios_after_crash !=
                             crash.recomputed_scenarios)) {
                        throw std::runtime_error(
                            "crash recovery counts changed across repetitions");
                    }
                    recovery_computed_scenarios =
                        crash.recovery_computed_scenarios;
                    recomputed_scenarios_after_crash =
                        crash.recomputed_scenarios;
                }

                const double durable_seconds = median(std::move(durations));
                const double durable_rate =
                    static_cast<double>(spec.total_scenarios) / durable_seconds;
                const std::uint64_t block_count =
                    1U + (spec.total_scenarios - 1U) / block_size;
                const std::uint64_t max_recomputed = std::min(
                    spec.total_scenarios,
                    interval > std::numeric_limits<std::uint64_t>::max() /
                                   block_size
                        ? spec.total_scenarios
                        : interval * block_size);
                std::cout << spec.total_scenarios << ','
                          << spec.num_time_steps << ',' << repeats << ','
                          << block_size << ',' << interval << ',' << workers
                          << ',' << block_count << ',' << non_durable_seconds
                          << ',' << durable_seconds << ',' << durable_rate << ','
                          << 100.0 * (1.0 - durable_rate / non_durable_rate) << ','
                          << median(std::move(cpu_utilization)) << ','
                          << median(std::move(commit_p50)) << ','
                          << median(std::move(commit_p95)) << ','
                          << median(std::move(commit_p99)) << ','
                          << median(std::move(persist_p50)) << ','
                          << median(std::move(persist_p95)) << ','
                          << median(std::move(persist_p99)) << ','
                          << median(std::move(checkpoint_p50)) << ','
                          << median(std::move(checkpoint_p95)) << ','
                          << median(std::move(checkpoint_p99)) << ','
                          << median(std::move(durable_open)) << ','
                          << median(std::move(restart_seconds)) << ','
                          << median(std::move(restart_open)) << ','
                          << 1U << ','
                          << median(std::move(crash_recovery_seconds)) << ','
                          << median(std::move(crash_recovery_open)) << ','
                          << recovery_computed_scenarios << ','
                          << recomputed_scenarios_after_crash << ','
                          << max_recomputed << ','
                          << last_metrics.durable_io.result_files_installed << ','
                          << last_metrics.durable_io.manifest_files_installed << ','
                          << median(std::move(bytes_written)) << ','
                          << last_metrics.checkpoint_samples_dropped << ','
                          << clean.aggregate.mean << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
