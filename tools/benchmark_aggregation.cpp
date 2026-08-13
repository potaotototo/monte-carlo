#include "mc/engine.hpp"
#include "mc/model.hpp"
#include "mc/parse.hpp"
#include "mc/run_spec.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct StrategyResult {
    double mean = 0.0;
    std::size_t workers_used = 0U;
    bool atomic_lock_free = false;
};

struct alignas(64) PaddedAggregate {
    mc::AggregateStats value;
};

static_assert(sizeof(PaddedAggregate) % 64U == 0U);

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

void atomic_add_relaxed(std::atomic<double>& target, double value) noexcept {
    double observed = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(
        observed, observed + value,
        std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

std::vector<std::size_t> worker_counts(std::size_t maximum) {
    std::vector<std::size_t> values{1U};
    for (const std::size_t candidate : {2U, 4U, 8U}) {
        if (candidate <= maximum) {
            values.push_back(candidate);
        }
    }
    if (maximum > values.back()) {
        values.push_back(maximum);
    }
    return values;
}

template <typename Function>
void run_static_workers(const mc::RunSpec& spec,
                        std::size_t worker_count,
                        Function&& function) {
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    std::mutex error_mutex;
    std::exception_ptr first_error;
    try {
        for (std::size_t worker = 0U; worker < worker_count; ++worker) {
            workers.emplace_back([&, worker] {
                try {
                    const std::uint64_t start =
                        spec.total_scenarios * worker / worker_count;
                    const std::uint64_t end =
                        spec.total_scenarios * (worker + 1U) / worker_count;
                    const mc::GbmKernel kernel(spec);
                    function(worker, start, end, kernel);
                } catch (...) {
                    std::lock_guard lock(error_mutex);
                    if (!first_error) {
                        first_error = std::current_exception();
                    }
                }
            });
        }
    } catch (...) {
        for (std::thread& worker : workers) {
            worker.join();
        }
        throw;
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    if (first_error) {
        std::rethrow_exception(first_error);
    }
}

StrategyResult run_global_mutex(const mc::RunSpec& spec,
                                std::size_t requested_workers) {
    const std::size_t workers = std::min<std::size_t>(
        requested_workers, static_cast<std::size_t>(spec.total_scenarios));
    mc::AggregateStats aggregate;
    std::mutex aggregate_mutex;
    run_static_workers(spec, workers,
        [&](std::size_t, std::uint64_t start, std::uint64_t end,
            const mc::GbmKernel& kernel) {
            for (std::uint64_t scenario = start; scenario < end; ++scenario) {
                const double payoff = kernel.discounted_payoff(scenario);
                std::lock_guard lock(aggregate_mutex);
                aggregate.add(payoff);
            }
        });
    return {aggregate.mean, workers, false};
}

StrategyResult run_atomic_sum(const mc::RunSpec& spec,
                              std::size_t requested_workers) {
    const std::size_t workers = std::min<std::size_t>(
        requested_workers, static_cast<std::size_t>(spec.total_scenarios));
    std::atomic<double> sum{0.0};
    const bool lock_free = sum.is_lock_free();
    run_static_workers(spec, workers,
        [&](std::size_t, std::uint64_t start, std::uint64_t end,
            const mc::GbmKernel& kernel) {
            for (std::uint64_t scenario = start; scenario < end; ++scenario) {
                atomic_add_relaxed(
                    sum, kernel.discounted_payoff(scenario));
            }
        });
    return {sum.load(std::memory_order_relaxed) /
                static_cast<double>(spec.total_scenarios),
            workers, lock_free};
}

StrategyResult run_worker_local(const mc::RunSpec& spec,
                                std::size_t requested_workers,
                                bool padded) {
    const std::size_t workers = std::min<std::size_t>(
        requested_workers, static_cast<std::size_t>(spec.total_scenarios));
    std::vector<mc::AggregateStats> local;
    std::vector<PaddedAggregate> padded_local;
    if (padded) {
        padded_local.resize(workers);
    } else {
        local.resize(workers);
    }
    run_static_workers(spec, workers,
        [&](std::size_t worker, std::uint64_t start, std::uint64_t end,
            const mc::GbmKernel& kernel) {
            mc::AggregateStats& aggregate =
                padded ? padded_local[worker].value : local[worker];
            for (std::uint64_t scenario = start; scenario < end; ++scenario) {
                aggregate.add(kernel.discounted_payoff(scenario));
            }
        });
    mc::AggregateStats aggregate;
    for (std::size_t worker = 0U; worker < workers; ++worker) {
        aggregate = mc::merge(
            aggregate,
            padded ? padded_local[worker].value : local[worker]);
    }
    return {aggregate.mean, workers, false};
}

StrategyResult run_deterministic_tree(const mc::RunSpec& spec,
                                      std::size_t workers,
                                      std::uint64_t block_size) {
    mc::EngineConfig config;
    config.worker_count = workers;
    config.block_size = block_size;
    const mc::RunResult result = mc::run_parallel(spec, config);
    return {result.aggregate.mean, result.workers_used, false};
}

void print_help() {
    std::cout
        << "Usage: benchmark_aggregation [options]\n"
        << "  --scenarios N       scenarios per measured run (default 1000000)\n"
        << "  --steps N           GBM time steps (default 1)\n"
        << "  --repeats N         measured repetitions (default 3)\n"
        << "  --max-workers N     largest worker count (default min(8, hardware))\n"
        << "  --block-size N      production tree block size (default 2048)\n"
        << "  --help              show this message\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        mc::RunSpec spec;
        spec.total_scenarios = 1'000'000U;
        std::size_t repeats = 3U;
        const std::size_t hardware =
            std::max<std::size_t>(1U, std::thread::hardware_concurrency());
        std::size_t max_workers = std::min<std::size_t>(8U, hardware);
        std::uint64_t block_size = 2'048U;

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
            } else if (argument == "--repeats") {
                repeats = mc::parse_size(
                    require_value(argc, argv, index), "repeats");
            } else if (argument == "--max-workers") {
                max_workers = mc::parse_size(
                    require_value(argc, argv, index), "max-workers");
            } else if (argument == "--block-size") {
                block_size = mc::parse_u64(
                    require_value(argc, argv, index), "block-size");
            } else {
                throw std::invalid_argument("unknown argument: " +
                                            std::string(argument));
            }
        }
        if (repeats == 0U || max_workers == 0U || block_size == 0U) {
            throw std::invalid_argument(
                "repeats, max-workers, and block-size must be positive");
        }
        spec.validate();
        mc::EngineConfig validation_config;
        validation_config.worker_count = max_workers;
        validation_config.block_size = block_size;
        validation_config.validate(spec);

        mc::RunSpec warmup = spec;
        warmup.total_scenarios =
            std::min<std::uint64_t>(spec.total_scenarios, 100'000U);
        static_cast<void>(run_deterministic_tree(warmup, 1U, block_size));

        std::cout
            << "scenarios,time_steps,repeats,strategy,block_size,workers_requested,"
               "workers_used,median_seconds,"
               "scenarios_per_second,speedup,parallel_efficiency,cpu_utilization_percent,"
               "price,atomic_lock_free\n";
        std::cout << std::fixed << std::setprecision(9);

        for (const std::string_view strategy :
             {"global_mutex", "atomic_sum", "worker_local",
              "padded_worker_local", "deterministic_tree"}) {
            double single_worker_rate = 0.0;
            for (const std::size_t workers : worker_counts(max_workers)) {
                std::vector<double> durations;
                std::vector<double> cpu_utilization;
                durations.reserve(repeats);
                cpu_utilization.reserve(repeats);
                StrategyResult last;
                for (std::size_t repeat = 0U; repeat < repeats; ++repeat) {
                    const double cpu_started = process_cpu_seconds();
                    const auto started = std::chrono::steady_clock::now();
                    if (strategy == "global_mutex") {
                        last = run_global_mutex(spec, workers);
                    } else if (strategy == "atomic_sum") {
                        last = run_atomic_sum(spec, workers);
                    } else if (strategy == "worker_local") {
                        last = run_worker_local(spec, workers, false);
                    } else if (strategy == "padded_worker_local") {
                        last = run_worker_local(spec, workers, true);
                    } else {
                        last = run_deterministic_tree(
                            spec, workers, block_size);
                    }
                    const auto stopped = std::chrono::steady_clock::now();
                    const double seconds =
                        std::chrono::duration<double>(stopped - started).count();
                    durations.push_back(seconds);
                    cpu_utilization.push_back(
                        100.0 * (process_cpu_seconds() - cpu_started) / seconds);
                }
                const double seconds = median(std::move(durations));
                const double rate =
                    static_cast<double>(spec.total_scenarios) / seconds;
                if (workers == 1U) {
                    single_worker_rate = rate;
                }
                const double speedup = rate / single_worker_rate;
                const double efficiency =
                    speedup / static_cast<double>(last.workers_used);
                std::cout << spec.total_scenarios << ','
                          << spec.num_time_steps << ',' << repeats << ','
                          << strategy << ','
                          << (strategy == "deterministic_tree" ? block_size : 0U)
                          << ',' << workers << ',' << last.workers_used << ','
                          << seconds << ',' << rate << ',' << speedup << ','
                          << efficiency << ','
                          << median(std::move(cpu_utilization)) << ','
                          << last.mean << ','
                          << (last.atomic_lock_free ? "true" : "false")
                          << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
