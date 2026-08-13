#include "mc/engine.hpp"
#include "mc/codec.hpp"
#include "mc/identity.hpp"
#include "mc/model.hpp"
#include "mc/parse.hpp"
#include "mc/persistence.hpp"
#include "mc/run_spec.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
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

void print_help() {
    std::cout
        << "Usage: run_simulation [options]\n"
        << "  --scenarios N       scenario count (default 100000)\n"
        << "  --steps N           time steps (default 1)\n"
        << "  --workers N         worker threads (default hardware concurrency)\n"
        << "  --block-size N      scenarios per deterministic block (default 2048)\n"
        << "  --max-blocks N      safety limit for materialized blocks (default 1000000)\n"
        << "  --run-dir PATH      enable R2 durable checkpoints and recovery\n"
        << "  --checkpoint-blocks N  full-manifest cadence (default 0: automatic)\n"
        << "  --max-storage-bytes N  run-store byte limit (default 64 GiB)\n"
        << "  --max-storage-files N  run-store regular-file limit (default 2000100)\n"
        << "  --min-free-bytes N     free space retained before each write (default 64 MiB)\n"
        << "  --max-manifest-bytes N maximum accepted manifest size (default 128 MiB)\n"
        << "  --seed N            global Philox key (default 1)\n"
        << "  --payoff TYPE       european or asian\n"
        << "  --spot X            initial asset price\n"
        << "  --strike X          option strike\n"
        << "  --rate X            continuously compounded risk-free rate\n"
        << "  --volatility X      annualized volatility\n"
        << "  --maturity X        maturity in years\n"
        << "  --antithetic        aggregate antithetic pair-means\n"
        << "  --metrics           emit opt-in R4 runtime and durable-I/O metrics\n"
        << "  --help              show this message\n";
}

void print_optional_ns(const std::optional<std::uint64_t>& value) {
    if (value.has_value()) {
        std::cout << *value;
    } else {
        std::cout << "null";
    }
}

void print_latency_summary(
    std::string_view name,
    const std::vector<std::uint64_t>& samples,
    bool trailing_comma) {
    std::cout << "    \"" << name << "\": {\"p50\": ";
    print_optional_ns(mc::latency_percentile_ns(samples, 0.50));
    std::cout << ", \"p95\": ";
    print_optional_ns(mc::latency_percentile_ns(samples, 0.95));
    std::cout << ", \"p99\": ";
    print_optional_ns(mc::latency_percentile_ns(samples, 0.99));
    std::cout << '}';
    if (trailing_comma) {
        std::cout << ',';
    }
    std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        mc::RunSpec spec;
        mc::EngineConfig config;
        mc::RunStoreConfig store_config;
        bool durable = false;
        bool store_policy_set = false;
        bool metrics_enabled = false;
        config.worker_count = std::max(1U, std::thread::hardware_concurrency());

        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--help") {
                print_help();
                return 0;
            }
            if (argument == "--antithetic") {
                spec.antithetic = true;
            } else if (argument == "--metrics") {
                metrics_enabled = true;
            } else if (argument == "--scenarios") {
                spec.total_scenarios =
                    mc::parse_u64(require_value(argc, argv, index), "scenarios");
            } else if (argument == "--steps") {
                const std::uint64_t value =
                    mc::parse_u64(require_value(argc, argv, index), "steps");
                if (value > mc::kMaxTimeSteps) {
                    throw std::invalid_argument("steps exceeds RNG layout v1");
                }
                spec.num_time_steps = static_cast<std::uint32_t>(value);
            } else if (argument == "--workers") {
                config.worker_count =
                    mc::parse_size(require_value(argc, argv, index), "workers");
            } else if (argument == "--block-size") {
                config.block_size =
                    mc::parse_u64(require_value(argc, argv, index), "block-size");
            } else if (argument == "--max-blocks") {
                config.max_materialized_blocks =
                    mc::parse_u64(require_value(argc, argv, index), "max-blocks");
            } else if (argument == "--run-dir") {
                store_config.run_directory = require_value(argc, argv, index);
                durable = true;
            } else if (argument == "--checkpoint-blocks") {
                store_policy_set = true;
                store_config.checkpoint_interval_blocks = mc::parse_u64(
                    require_value(argc, argv, index), "checkpoint-blocks");
            } else if (argument == "--max-storage-bytes") {
                store_policy_set = true;
                store_config.max_storage_bytes = mc::parse_u64(
                    require_value(argc, argv, index), "max-storage-bytes");
            } else if (argument == "--max-storage-files") {
                store_policy_set = true;
                store_config.max_storage_files = mc::parse_u64(
                    require_value(argc, argv, index), "max-storage-files");
            } else if (argument == "--min-free-bytes") {
                store_policy_set = true;
                store_config.min_free_space_bytes = mc::parse_u64(
                    require_value(argc, argv, index), "min-free-bytes");
            } else if (argument == "--max-manifest-bytes") {
                store_policy_set = true;
                store_config.max_manifest_bytes = mc::parse_u64(
                    require_value(argc, argv, index), "max-manifest-bytes");
            } else if (argument == "--seed") {
                spec.global_seed =
                    mc::parse_u64(require_value(argc, argv, index), "seed");
            } else if (argument == "--payoff") {
                const std::string payoff = require_value(argc, argv, index);
                if (payoff == "european") {
                    spec.payoff_type = mc::PayoffType::EuropeanCall;
                } else if (payoff == "asian") {
                    spec.payoff_type = mc::PayoffType::AsianCall;
                } else {
                    throw std::invalid_argument("payoff must be european or asian");
                }
            } else if (argument == "--spot") {
                spec.spot =
                    mc::parse_finite_double(require_value(argc, argv, index), "spot");
            } else if (argument == "--strike") {
                spec.strike =
                    mc::parse_finite_double(require_value(argc, argv, index), "strike");
            } else if (argument == "--rate") {
                spec.rate =
                    mc::parse_finite_double(require_value(argc, argv, index), "rate");
            } else if (argument == "--volatility") {
                spec.volatility = mc::parse_finite_double(
                    require_value(argc, argv, index), "volatility");
            } else if (argument == "--maturity") {
                spec.maturity = mc::parse_finite_double(
                    require_value(argc, argv, index), "maturity");
            } else {
                throw std::invalid_argument(std::string("unknown argument: ") +
                                            std::string(argument));
            }
        }
        if (store_policy_set && !durable) {
            throw std::invalid_argument(
                "durable storage policy options require --run-dir");
        }

        const auto started = std::chrono::steady_clock::now();
        std::optional<mc::DurableRunResult> durable_result;
        mc::RuntimeMetrics runtime_metrics;
        mc::RuntimeMetrics* const metrics =
            metrics_enabled ? &runtime_metrics : nullptr;
        const mc::RunResult result = [&] {
            if (durable) {
                durable_result =
                    mc::run_parallel_durable(spec, config, store_config, metrics);
                return durable_result->run_result;
            }
            return mc::run_parallel(spec, config, metrics);
        }();
        const auto stopped = std::chrono::steady_clock::now();
        const double seconds =
            std::chrono::duration<double>(stopped - started).count();
        if (!(seconds > 0.0)) {
            throw std::runtime_error("steady clock produced a non-positive duration");
        }
        const std::optional<double> standard_error =
            result.aggregate.standard_error();
        const std::optional<double> confidence_low = result.confidence_low();
        const std::optional<double> confidence_high = result.confidence_high();
        const mc::BuildIdentity build = mc::current_build_identity();

        std::cout << std::fixed << std::setprecision(12);
        std::cout << "{\n"
                  << "  \"engine_version\": " << spec.engine_version << ",\n"
                  << "  \"rng_version\": " << spec.rng_version << ",\n"
                  << "  \"stats_schema_version\": " << spec.stats_schema_version << ",\n"
                  << "  \"run_spec_hash\": \"" << mc::to_hex(mc::run_spec_hash(spec))
                  << "\",\n"
                  << "  \"execution_layout_hash\": \""
                  << mc::to_hex(mc::execution_layout_hash(spec, config)) << "\",\n"
                  << "  \"build_fingerprint\": \"" << mc::to_hex(build.hash)
                  << "\",\n"
                  << "  \"durable\": " << (durable ? "true" : "false");
        if (durable_result.has_value()) {
            std::cout << ",\n  \"run_id\": \""
                      << mc::to_hex(durable_result->run_id) << "\",\n"
                      << "  \"manifest_sequence\": "
                      << durable_result->manifest_sequence << ",\n"
                      << "  \"run_incarnation\": "
                      << durable_result->run_incarnation << ",\n"
                      << "  \"resumed\": "
                      << (durable_result->resumed ? "true" : "false") << ",\n"
                      << "  \"recovered_blocks\": "
                      << durable_result->recovered_blocks << ",\n"
                      << "  \"computed_blocks\": "
                      << durable_result->computed_blocks << ",\n"
                      << "  \"recovered_scenarios\": "
                      << durable_result->recovered_scenarios << ",\n"
                      << "  \"computed_scenarios\": "
                      << durable_result->computed_scenarios;
        }
        std::cout << ",\n"
                  << "  \"model\": \"" << mc::to_string(spec.model_type) << "\",\n"
                  << "  \"payoff\": \"" << mc::to_string(spec.payoff_type) << "\",\n"
                  << "  \"scenarios\": " << result.scenarios_processed << ",\n"
                  << "  \"observations\": " << result.aggregate.n << ",\n"
                  << "  \"blocks\": " << result.block_count << ",\n"
                  << "  \"block_size\": " << config.block_size << ",\n"
                  << "  \"workers_requested\": " << config.worker_count << ",\n"
                  << "  \"workers_used\": " << result.workers_used << ",\n"
                  << "  \"antithetic\": " << (spec.antithetic ? "true" : "false") << ",\n"
                  << "  \"price\": " << result.aggregate.mean << ",\n"
                  << "  \"standard_error\": ";
        if (standard_error.has_value()) {
            std::cout << *standard_error;
        } else {
            std::cout << "null";
        }
        std::cout << ",\n  \"confidence_95\": ";
        if (confidence_low.has_value() && confidence_high.has_value()) {
            std::cout << '[' << *confidence_low << ", " << *confidence_high << ']';
        } else {
            std::cout << "null";
        }
        std::cout << ",\n  \"confidence_method\": "
                  << (confidence_low.has_value()
                          ? "\"normal_approximation\""
                          : "null")
                  << ",\n"
                  << "  \"elapsed_seconds\": " << seconds << ",\n"
                  << "  \"scenarios_per_second\": "
                  << static_cast<double>(
                         durable_result.has_value()
                             ? durable_result->computed_scenarios
                             : result.scenarios_processed) /
                         seconds;

        if (spec.payoff_type == mc::PayoffType::EuropeanCall) {
            const double analytic = mc::black_scholes_call_price(
                spec.spot, spec.strike, spec.rate, spec.volatility, spec.maturity);
            std::cout << ",\n  \"black_scholes_price\": " << analytic
                      << ",\n  \"analytic_error\": " << result.aggregate.mean - analytic;
        }
        if (metrics_enabled) {
            std::cout << ",\n  \"metrics\": {\n"
                      << "    \"total_elapsed_ns\": "
                      << runtime_metrics.total_elapsed_ns << ",\n"
                      << "    \"durable_open_ns\": "
                      << runtime_metrics.durable_open_ns << ",\n"
                      << "    \"scheduler_assignment_wait_ns\": "
                      << runtime_metrics.scheduler_assignment_wait_ns << ",\n"
                      << "    \"coordinator_completion_wait_ns\": "
                      << runtime_metrics.coordinator_completion_wait_ns << ",\n"
                      << "    \"coordinator_consume_ns\": "
                      << runtime_metrics.coordinator_consume_ns << ",\n"
                      << "    \"fixed_tree_reduce_ns\": "
                      << runtime_metrics.fixed_tree_reduce_ns << ",\n"
                      << "    \"max_assignment_queue_depth\": "
                      << runtime_metrics.max_assignment_queue_depth << ",\n"
                      << "    \"max_completion_queue_depth\": "
                      << runtime_metrics.max_completion_queue_depth << ",\n";
            print_latency_summary("block_compute_ns",
                                  runtime_metrics.block_compute_ns, true);
            print_latency_summary("result_persist_ns",
                                  runtime_metrics.result_persist_ns, true);
            print_latency_summary("checkpoint_ns",
                                  runtime_metrics.checkpoint_ns, true);
            std::cout << "    \"durable_io\": {\n"
                      << "      \"metadata_files_installed\": "
                      << runtime_metrics.durable_io.metadata_files_installed
                      << ",\n      \"result_files_installed\": "
                      << runtime_metrics.durable_io.result_files_installed
                      << ",\n      \"manifest_files_installed\": "
                      << runtime_metrics.durable_io.manifest_files_installed
                      << ",\n      \"bytes_written\": "
                      << runtime_metrics.durable_io.bytes_written
                      << ",\n      \"write_ns\": "
                      << runtime_metrics.durable_io.write_ns
                      << ",\n      \"file_fsync_ns\": "
                      << runtime_metrics.durable_io.file_fsync_ns
                      << ",\n      \"rename_ns\": "
                      << runtime_metrics.durable_io.rename_ns
                      << ",\n      \"directory_fsync_ns\": "
                      << runtime_metrics.durable_io.directory_fsync_ns
                      << "\n    },\n"
                      << "    \"workers\": [";
            for (std::size_t index = 0;
                 index < runtime_metrics.workers.size(); ++index) {
                if (index != 0U) {
                    std::cout << ',';
                }
                const mc::WorkerMetrics& worker = runtime_metrics.workers[index];
                std::cout << "\n      {\"worker\": " << index
                          << ", \"blocks_completed\": "
                          << worker.blocks_completed
                          << ", \"scenarios_completed\": "
                          << worker.scenarios_completed
                          << ", \"assignment_wait_ns\": "
                          << worker.assignment_wait_ns
                          << ", \"compute_ns\": " << worker.compute_ns
                          << ", \"completion_queue_wait_ns\": "
                          << worker.completion_queue_wait_ns << '}';
            }
            if (!runtime_metrics.workers.empty()) {
                std::cout << "\n    ";
            }
            std::cout << "]\n  }";
        }
        std::cout << "\n}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
