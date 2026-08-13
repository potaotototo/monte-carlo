#include "mc/engine.hpp"
#include "mc/parse.hpp"
#include "mc/run_spec.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

void print_help() {
    std::cout
        << "Usage: benchmark_scaling [options]\n"
        << "  --scenarios N       scenarios per measured run (default 1000000)\n"
        << "  --steps N           GBM time steps (default 1)\n"
        << "  --repeats N         measured repetitions (default 3)\n"
        << "  --max-workers N     largest worker count (default min(8, hardware))\n"
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

        std::cout << "block_size,workers_requested,workers_used,median_seconds,scenarios_per_second,"
                     "speedup,parallel_efficiency,price,standard_error\n";
        std::cout << std::fixed << std::setprecision(9);
        for (const std::uint64_t block_size : {512U, 2'048U, 8'192U, 32'768U}) {
            double single_worker_rate = 0.0;
            for (const std::size_t workers : worker_counts(max_workers)) {
                mc::EngineConfig config;
                config.worker_count = workers;
                config.block_size = block_size;

                std::vector<double> durations;
                durations.reserve(repeats);
                mc::RunResult last_result;
                for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
                    const auto started = std::chrono::steady_clock::now();
                    last_result = mc::run_parallel(spec, config);
                    const auto stopped = std::chrono::steady_clock::now();
                    durations.push_back(
                        std::chrono::duration<double>(stopped - started).count());
                }
                const double seconds = median(std::move(durations));
                const double rate =
                    static_cast<double>(spec.total_scenarios) / seconds;
                if (workers == 1U) {
                    single_worker_rate = rate;
                }
                const double speedup = rate / single_worker_rate;
                const double efficiency = speedup / static_cast<double>(workers);
                const std::optional<double> standard_error =
                    last_result.aggregate.standard_error();
                if (!standard_error.has_value()) {
                    throw std::runtime_error(
                        "benchmark requires at least two statistical observations");
                }
                std::cout << block_size << ',' << workers << ','
                          << last_result.workers_used << ',' << seconds << ','
                          << rate << ',' << speedup << ',' << efficiency << ','
                          << last_result.aggregate.mean << ','
                          << *standard_error << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
