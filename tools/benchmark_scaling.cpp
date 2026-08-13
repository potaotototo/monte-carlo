#include "mc/engine.hpp"
#include "mc/parse.hpp"
#include "mc/run_spec.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::string require_value(int argc, char** argv, int& index) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") + argv[index]);
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

double required_metric(
    const std::optional<std::uint64_t>& value,
    std::string_view name) {
    if (!value.has_value()) {
        throw std::runtime_error("benchmark produced no samples for " +
                                 std::string(name));
    }
    return static_cast<double>(*value);
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

std::vector<std::size_t> worker_counts(std::size_t maximum) {
    std::vector<std::size_t> values{1};
    for (std::size_t candidate : {2U, 4U, 8U}) {
        if (candidate <= maximum) {
            values.push_back(candidate);
        }
    }
    if (maximum > values.back()) {
        values.push_back(maximum);
    }
    return values;
}

std::vector<std::size_t> parse_size_list(std::string_view text,
                                         std::string_view field) {
    std::vector<std::size_t> values;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view token = text.substr(0U, comma);
        if (token.empty()) {
            throw std::invalid_argument(std::string(field) +
                                        " contains an empty value");
        }
        const std::size_t value = mc::parse_size(token, field);
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

void print_help() {
    std::cout
        << "Usage: benchmark_scaling [options]\n"
        << "  --scenarios N       scenarios per measured run (default 1000000)\n"
        << "  --steps N           GBM time steps (default 1)\n"
        << "  --repeats N         measured repetitions (default 3)\n"
        << "  --max-workers N     largest worker count (default min(8, hardware))\n"
        << "  --queue-capacities LIST  comma-separated shared queue capacities; 0 is auto\n"
        << "  --help              show this message\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        mc::RunSpec spec;
        spec.total_scenarios = 1'000'000;
        std::size_t repeats = 3;
        const std::size_t hardware =
            std::max<std::size_t>(1, std::thread::hardware_concurrency());
        std::size_t max_workers = std::min<std::size_t>(8, hardware);
        std::vector<std::size_t> queue_capacities{0U};

        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--help") {
                print_help();
                return 0;
            }
            if (argument == "--scenarios") {
                spec.total_scenarios =
                    mc::parse_u64(require_value(argc, argv, index), "scenarios");
            } else if (argument == "--steps") {
                const std::uint64_t value =
                    mc::parse_u64(require_value(argc, argv, index), "steps");
                if (value > mc::kMaxTimeSteps) {
                    throw std::invalid_argument("steps exceeds RNG layout v1");
                }
                spec.num_time_steps = static_cast<std::uint32_t>(value);
            } else if (argument == "--repeats") {
                repeats =
                    mc::parse_size(require_value(argc, argv, index), "repeats");
            } else if (argument == "--max-workers") {
                max_workers = mc::parse_size(
                    require_value(argc, argv, index), "max-workers");
            } else if (argument == "--queue-capacities") {
                queue_capacities = parse_size_list(
                    require_value(argc, argv, index), "queue-capacities");
            } else {
                throw std::invalid_argument(std::string("unknown argument: ") +
                                            std::string(argument));
            }
        }
        if (repeats == 0 || max_workers == 0) {
            throw std::invalid_argument("repeats and max-workers must be positive");
        }
        spec.validate();

        mc::RunSpec warmup_spec = spec;
        warmup_spec.total_scenarios =
            std::min<std::uint64_t>(spec.total_scenarios, 100'000U);
        mc::EngineConfig warmup_config;
        warmup_config.worker_count = 1;
        warmup_config.block_size = 2'048;
        static_cast<void>(mc::run_parallel(warmup_spec, warmup_config));

        std::cout << "scenarios,time_steps,repeats,block_size,queue_capacity_requested,"
                     "queue_capacity_resolved,workers_requested,workers_used,"
                     "metrics_median_seconds,metrics_scenarios_per_second,"
                     "unobserved_median_seconds,unobserved_scenarios_per_second,"
                     "metrics_overhead_percent,metrics_speedup,metrics_parallel_efficiency,"
                     "unobserved_speedup,unobserved_parallel_efficiency,"
                     "cpu_utilization_percent,price,standard_error,block_compute_p50_ns,"
                     "block_compute_p95_ns,block_compute_p99_ns,block_commit_p50_ns,"
                     "block_commit_p95_ns,block_commit_p99_ns,assignment_queue_peak,"
                     "completion_queue_peak,scheduler_blocked_ns,coordinator_blocked_ns,"
                     "coordinator_consume_ns,coordinator_blocks_per_second,"
                     "max_reduction_backlog_blocks,max_reduction_backlog_bytes\n";
        std::cout << std::fixed << std::setprecision(9);
        for (const std::uint64_t block_size :
            {512U, 2'048U, 8'192U, 32'768U}) {
            for (const std::size_t queue_capacity : queue_capacities) {
                double single_worker_metrics_rate = 0.0;
                double single_worker_unobserved_rate = 0.0;
                for (const std::size_t workers : worker_counts(max_workers)) {
                mc::EngineConfig config;
                config.worker_count = workers;
                config.block_size = block_size;
                config.assignment_queue_capacity = queue_capacity;
                config.completion_queue_capacity = queue_capacity;

                std::vector<double> durations;
                std::vector<double> unobserved_durations;
                std::vector<double> cpu_utilization;
                std::vector<double> block_p50;
                std::vector<double> block_p95;
                std::vector<double> block_p99;
                std::vector<double> commit_p50;
                std::vector<double> commit_p95;
                std::vector<double> commit_p99;
                std::vector<double> assignment_queue_peak;
                std::vector<double> completion_queue_peak;
                std::vector<double> scheduler_assignment_wait;
                std::vector<double> coordinator_completion_wait;
                std::vector<double> coordinator_consume;
                std::vector<double> coordinator_rate;
                durations.reserve(repeats);
                unobserved_durations.reserve(repeats);
                cpu_utilization.reserve(repeats);
                block_p50.reserve(repeats);
                block_p95.reserve(repeats);
                block_p99.reserve(repeats);
                commit_p50.reserve(repeats);
                commit_p95.reserve(repeats);
                commit_p99.reserve(repeats);
                assignment_queue_peak.reserve(repeats);
                completion_queue_peak.reserve(repeats);
                scheduler_assignment_wait.reserve(repeats);
                coordinator_completion_wait.reserve(repeats);
                coordinator_consume.reserve(repeats);
                coordinator_rate.reserve(repeats);
                mc::RunResult last_result;
                mc::RunResult last_unobserved_result;
                mc::RuntimeMetrics last_metrics;
                for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
                    const auto measure_unobserved = [&] {
                        const auto started = std::chrono::steady_clock::now();
                        last_unobserved_result =
                            mc::run_parallel(spec, config);
                        unobserved_durations.push_back(
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - started)
                                .count());
                    };
                    const auto measure_observed = [&] {
                        mc::RuntimeMetrics metrics;
                        const double cpu_started = process_cpu_seconds();
                        const auto started = std::chrono::steady_clock::now();
                        last_result = mc::run_parallel(spec, config, &metrics);
                        const double seconds = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - started).count();
                        durations.push_back(seconds);
                        cpu_utilization.push_back(
                            100.0 * (process_cpu_seconds() - cpu_started) /
                            seconds);
                        block_p50.push_back(required_metric(
                            mc::latency_percentile_ns(
                                metrics.block_compute_ns, 0.50),
                            "block p50"));
                        block_p95.push_back(required_metric(
                            mc::latency_percentile_ns(
                                metrics.block_compute_ns, 0.95),
                            "block p95"));
                        block_p99.push_back(required_metric(
                            mc::latency_percentile_ns(
                                metrics.block_compute_ns, 0.99),
                            "block p99"));
                        commit_p50.push_back(required_metric(
                            mc::latency_percentile_ns(
                                metrics.block_commit_ns, 0.50),
                            "commit p50"));
                        commit_p95.push_back(required_metric(
                            mc::latency_percentile_ns(
                                metrics.block_commit_ns, 0.95),
                            "commit p95"));
                        commit_p99.push_back(required_metric(
                            mc::latency_percentile_ns(
                                metrics.block_commit_ns, 0.99),
                            "commit p99"));
                        assignment_queue_peak.push_back(static_cast<double>(
                            metrics.max_assignment_queue_depth));
                        completion_queue_peak.push_back(static_cast<double>(
                            metrics.max_completion_queue_depth));
                        scheduler_assignment_wait.push_back(static_cast<double>(
                            metrics.scheduler_assignment_wait_ns));
                        coordinator_completion_wait.push_back(
                            static_cast<double>(
                                metrics.coordinator_completion_wait_ns));
                        coordinator_consume.push_back(static_cast<double>(
                            metrics.coordinator_consume_ns));
                        coordinator_rate.push_back(
                            metrics.coordinator_consume_ns == 0U
                                ? 0.0
                                : static_cast<double>(last_result.block_count) *
                                      1.0e9 /
                                      static_cast<double>(
                                          metrics.coordinator_consume_ns));
                        last_metrics = std::move(metrics);
                    };
                    if (repeat % 2U == 0U) {
                        measure_unobserved();
                        measure_observed();
                    } else {
                        measure_observed();
                        measure_unobserved();
                    }
                    if (!aggregates_equal(last_result.aggregate,
                                          last_unobserved_result.aggregate)) {
                        throw std::runtime_error(
                            "metrics changed the deterministic aggregate");
                    }
                }
                const double seconds = median(std::move(durations));
                const double rate =
                    static_cast<double>(spec.total_scenarios) / seconds;
                const double unobserved_seconds =
                    median(std::move(unobserved_durations));
                const double unobserved_rate =
                    static_cast<double>(spec.total_scenarios) /
                    unobserved_seconds;
                if (workers == 1U) {
                    single_worker_metrics_rate = rate;
                    single_worker_unobserved_rate = unobserved_rate;
                }
                const double metrics_speedup =
                    rate / single_worker_metrics_rate;
                const double metrics_efficiency =
                    metrics_speedup /
                    static_cast<double>(last_result.workers_used);
                const double unobserved_speedup =
                    unobserved_rate / single_worker_unobserved_rate;
                const double unobserved_efficiency =
                    unobserved_speedup /
                    static_cast<double>(last_unobserved_result.workers_used);
                const std::optional<double> standard_error =
                    last_result.aggregate.standard_error();
                if (!standard_error.has_value()) {
                    throw std::runtime_error(
                        "benchmark requires at least two statistical observations");
                }
                const std::size_t resolved_capacity =
                    queue_capacity == 0U
                        ? std::max<std::size_t>(
                              1U, last_result.workers_used * 2U)
                        : queue_capacity;
                std::cout << spec.total_scenarios << ','
                          << spec.num_time_steps << ',' << repeats << ','
                          << block_size << ',' << queue_capacity << ','
                          << resolved_capacity << ',' << workers << ','
                          << last_result.workers_used << ',' << seconds << ','
                          << rate << ',' << unobserved_seconds << ','
                          << unobserved_rate << ','
                          << 100.0 * (1.0 - rate / unobserved_rate) << ','
                          << metrics_speedup << ',' << metrics_efficiency << ','
                          << unobserved_speedup << ','
                          << unobserved_efficiency << ','
                          << median(std::move(cpu_utilization)) << ','
                          << last_result.aggregate.mean << ','
                          << *standard_error << ','
                          << median(std::move(block_p50)) << ','
                          << median(std::move(block_p95)) << ','
                          << median(std::move(block_p99)) << ','
                          << median(std::move(commit_p50)) << ','
                          << median(std::move(commit_p95)) << ','
                          << median(std::move(commit_p99)) << ','
                          << median(std::move(assignment_queue_peak)) << ','
                          << median(std::move(completion_queue_peak)) << ','
                          << median(std::move(scheduler_assignment_wait)) << ','
                          << median(std::move(coordinator_completion_wait)) << ','
                          << median(std::move(coordinator_consume)) << ','
                          << median(std::move(coordinator_rate)) << ','
                          << last_metrics.max_reduction_backlog_blocks << ','
                          << last_metrics.max_reduction_backlog_bytes
                          << '\n';
                }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
