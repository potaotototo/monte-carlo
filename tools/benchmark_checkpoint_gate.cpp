#include "mc/engine.hpp"
#include "mc/hash.hpp"
#include "mc/identity.hpp"
#include "mc/parse.hpp"
#include "mc/persistence.hpp"
#include "mc/run_spec.hpp"

#include "checkpoint_gate_stats.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

class GateWorkspace {
public:
    explicit GateWorkspace(const std::filesystem::path& parent) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error || !std::filesystem::is_directory(parent)) {
            throw std::runtime_error(
                "cannot create checkpoint-gate workspace parent");
        }
        for (std::uint32_t attempt = 0U; attempt < 100U; ++attempt) {
            root_ = parent /
                    ("mc-r4-checkpoint-gate-" +
                     std::to_string(static_cast<std::uint64_t>(::getpid())) +
                     "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(root_, error)) {
                return;
            }
            error.clear();
        }
        throw std::runtime_error(
            "cannot create unique checkpoint-gate workspace");
    }

    GateWorkspace(const GateWorkspace&) = delete;
    GateWorkspace& operator=(const GateWorkspace&) = delete;

    ~GateWorkspace() noexcept {
        if (root_.empty()) {
            return;
        }
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(root_, ignored));
    }

    [[nodiscard]] std::filesystem::path run_directory(
        std::string_view phase,
        std::size_t pair,
        std::string_view variant) const {
        return root_ / std::string(phase) /
               ("pair-" + std::to_string(pair)) / std::string(variant);
    }

    void cleanup_pair(std::string_view phase, std::size_t pair) const {
        const std::filesystem::path path =
            root_ / std::string(phase) / ("pair-" + std::to_string(pair));
        std::error_code error;
        static_cast<void>(std::filesystem::remove_all(path, error));
        if (error) {
            throw std::runtime_error(
                "cannot remove checkpoint-gate pair workspace: " +
                error.message());
        }
    }

    void cleanup() {
        std::error_code error;
        static_cast<void>(std::filesystem::remove_all(root_, error));
        if (error) {
            throw std::runtime_error(
                "cannot remove checkpoint-gate workspace: " +
                error.message());
        }
        root_.clear();
    }

private:
    std::filesystem::path root_;
};

struct Measurement {
    double seconds = 0.0;
    double scenarios_per_second = 0.0;
    mc::AggregateStats aggregate;
    std::size_t workers_used = 0U;
};

struct Trial {
    std::size_t pair = 0U;
    bool control_first = true;
    Measurement control;
    Measurement treatment;
};

std::string require_value(int argc, char** argv, int& index) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") +
                                    argv[index]);
    }
    ++index;
    return argv[index];
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

void cooldown(std::uint64_t milliseconds) {
    if (milliseconds != 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
    }
}

Measurement measure_durable(
    const mc::RunSpec& spec,
    const mc::EngineConfig& engine_config,
    std::uint64_t checkpoint_interval,
    const std::filesystem::path& run_directory) {
    mc::RunStoreConfig store_config;
    store_config.run_directory = run_directory;
    store_config.checkpoint_interval_blocks = checkpoint_interval;
    store_config.min_free_space_bytes = 0U;
    const auto started = std::chrono::steady_clock::now();
    const mc::DurableRunResult result = mc::run_parallel_durable(
        spec, engine_config, store_config);
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    if (!std::isfinite(seconds) || seconds <= 0.0 ||
        result.computed_scenarios != spec.total_scenarios ||
        result.recovered_scenarios != 0U) {
        throw std::runtime_error(
            "checkpoint-gate run produced invalid timing or work counts");
    }
    return {seconds,
            static_cast<double>(spec.total_scenarios) / seconds,
            result.run_result.aggregate,
            result.run_result.workers_used};
}

Trial measure_pair(const mc::RunSpec& spec,
                   const mc::EngineConfig& engine_config,
                   std::uint64_t control_interval,
                   std::uint64_t treatment_interval,
                   std::uint64_t cooldown_ms,
                   GateWorkspace& workspace,
                   std::string_view phase,
                   std::size_t pair,
                   bool control_first) {
    Trial trial;
    trial.pair = pair;
    trial.control_first = control_first;
    const auto run_control = [&] {
        cooldown(cooldown_ms);
        trial.control = measure_durable(
            spec, engine_config, control_interval,
            workspace.run_directory(phase, pair, "control"));
    };
    const auto run_treatment = [&] {
        cooldown(cooldown_ms);
        trial.treatment = measure_durable(
            spec, engine_config, treatment_interval,
            workspace.run_directory(phase, pair, "treatment"));
    };
    if (control_first) {
        run_control();
        run_treatment();
    } else {
        run_treatment();
        run_control();
    }
    if (!aggregates_equal(trial.control.aggregate,
                          trial.treatment.aggregate) ||
        trial.control.workers_used != trial.treatment.workers_used) {
        throw std::runtime_error(
            "checkpoint cadence changed the aggregate or active workers");
    }
    workspace.cleanup_pair(phase, pair);
    return trial;
}

void print_help() {
    std::cout
        << "Usage: benchmark_checkpoint_gate [options]\n"
        << "  --scenarios N       scenarios per run (default 100000000)\n"
        << "  --steps N           GBM time steps (default 1)\n"
        << "  --workers N         worker count (default min(8, hardware))\n"
        << "  --block-size N      scenarios per block (default 10000)\n"
        << "  --pairs N           measured AB/BA pairs (default 20)\n"
        << "  --warmup-pairs N    unreported warmup pairs (default 1)\n"
        << "  --control-interval N  final-only cadence (default 10000)\n"
        << "  --treatment-interval N sparse cadence (default 6000)\n"
        << "  --cooldown-ms N     pause before every run (default 1000)\n"
        << "  --bootstrap-samples N  resamples, minimum 1000 (default 10000)\n"
        << "  --bootstrap-seed N  deterministic resampling seed\n"
        << "  --max-loss-percent X  upper-CI gate (default 10)\n"
        << "  --max-drift-percent X control drift gate (default 5)\n"
        << "  --max-order-effect-pp X AB/BA median gap gate (default 5)\n"
        << "  --workspace PATH    parent for temporary run stores\n"
        << "  --require-pass      exit 2 when the evidence gate does not pass\n"
        << "  --help              show this message\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        mc::RunSpec spec;
        spec.total_scenarios = 100'000'000U;
        const std::size_t hardware =
            std::max<std::size_t>(1U, std::thread::hardware_concurrency());
        mc::EngineConfig engine_config;
        engine_config.worker_count = std::min<std::size_t>(8U, hardware);
        engine_config.block_size = 10'000U;
        std::size_t pairs = 20U;
        std::size_t warmup_pairs = 1U;
        std::uint64_t control_interval = 10'000U;
        std::uint64_t treatment_interval = 6'000U;
        std::uint64_t cooldown_ms = 1'000U;
        mc::tool::CheckpointGatePolicy policy;
        std::filesystem::path workspace_parent =
            std::filesystem::temp_directory_path();
        bool require_pass = false;

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
                    throw std::invalid_argument(
                        "steps exceeds RNG layout v1");
                }
                spec.num_time_steps = static_cast<std::uint32_t>(value);
            } else if (argument == "--workers") {
                engine_config.worker_count = mc::parse_size(
                    require_value(argc, argv, index), "workers");
            } else if (argument == "--block-size") {
                engine_config.block_size = mc::parse_u64(
                    require_value(argc, argv, index), "block-size");
            } else if (argument == "--pairs") {
                pairs = mc::parse_size(
                    require_value(argc, argv, index), "pairs");
            } else if (argument == "--warmup-pairs") {
                warmup_pairs = mc::parse_size(
                    require_value(argc, argv, index), "warmup-pairs");
            } else if (argument == "--control-interval") {
                control_interval = mc::parse_u64(
                    require_value(argc, argv, index), "control-interval");
            } else if (argument == "--treatment-interval") {
                treatment_interval = mc::parse_u64(
                    require_value(argc, argv, index),
                    "treatment-interval");
            } else if (argument == "--cooldown-ms") {
                cooldown_ms = mc::parse_u64(
                    require_value(argc, argv, index), "cooldown-ms");
            } else if (argument == "--bootstrap-samples") {
                policy.bootstrap_samples = mc::parse_size(
                    require_value(argc, argv, index),
                    "bootstrap-samples");
            } else if (argument == "--bootstrap-seed") {
                policy.bootstrap_seed = mc::parse_u64(
                    require_value(argc, argv, index), "bootstrap-seed");
            } else if (argument == "--max-loss-percent") {
                policy.maximum_loss_percent = mc::parse_finite_double(
                    require_value(argc, argv, index), "max-loss-percent");
            } else if (argument == "--max-drift-percent") {
                policy.maximum_control_drift_percent =
                    mc::parse_finite_double(
                        require_value(argc, argv, index),
                        "max-drift-percent");
            } else if (argument == "--max-order-effect-pp") {
                policy.maximum_order_effect_percentage_points =
                    mc::parse_finite_double(
                        require_value(argc, argv, index),
                        "max-order-effect-pp");
            } else if (argument == "--workspace") {
                workspace_parent = require_value(argc, argv, index);
            } else if (argument == "--require-pass") {
                require_pass = true;
            } else {
                throw std::invalid_argument(
                    "unknown argument: " + std::string(argument));
            }
        }

        policy.minimum_pairs = 20U;
        if (pairs < 2U || pairs > 100U || warmup_pairs > 10U ||
            cooldown_ms > 3'600'000U ||
            policy.bootstrap_samples < 1'000U ||
            policy.bootstrap_samples > 1'000'000U ||
            policy.maximum_loss_percent < 0.0 ||
            policy.maximum_control_drift_percent < 0.0 ||
            policy.maximum_order_effect_percentage_points < 0.0) {
            throw std::invalid_argument(
                "checkpoint gate policy or resource input is outside its "
                "safety limit");
        }
        spec.validate();
        engine_config.validate(spec);
        const std::uint64_t block_count =
            1U + (spec.total_scenarios - 1U) / engine_config.block_size;
        if (block_count > engine_config.max_materialized_blocks ||
            control_interval < block_count || treatment_interval == 0U ||
            treatment_interval >= block_count) {
            throw std::invalid_argument(
                "control must be final-only, treatment must be periodic, and "
                "the block count must fit the materialization limit");
        }

        GateWorkspace workspace(workspace_parent);
        mc::RunSpec warmup_spec = spec;
        warmup_spec.total_scenarios =
            std::min<std::uint64_t>(spec.total_scenarios, 100'000U);
        static_cast<void>(mc::run_parallel(warmup_spec, engine_config));
        for (std::size_t warmup = 0U; warmup < warmup_pairs; ++warmup) {
            static_cast<void>(measure_pair(
                spec, engine_config, control_interval, treatment_interval,
                cooldown_ms, workspace, "warmup", warmup,
                warmup % 2U == 0U));
        }

        std::vector<Trial> trials;
        std::vector<mc::tool::CheckpointGateObservation> observations;
        trials.reserve(pairs);
        observations.reserve(pairs);
        for (std::size_t pair = 0U; pair < pairs; ++pair) {
            Trial trial = measure_pair(
                spec, engine_config, control_interval, treatment_interval,
                cooldown_ms, workspace, "measured", pair,
                pair % 2U == 0U);
            observations.push_back({trial.control.scenarios_per_second,
                                    trial.treatment.scenarios_per_second,
                                    trial.control_first});
            trials.push_back(std::move(trial));
        }
        const mc::tool::CheckpointGateSummary summary =
            mc::tool::summarize_checkpoint_gate(observations, policy);
        const mc::BuildIdentity build = mc::current_build_identity();

        std::cout
            << "gate_schema_version,pair_index,order,measured_pairs,"
               "warmup_pairs,scenarios,"
               "time_steps,global_seed,model,payoff,spot,strike,rate,"
               "volatility,maturity,engine_version,rng_version,"
               "stats_schema_version,block_size,blocks,workers_requested,"
               "workers_used,"
               "control_checkpoint_interval,treatment_checkpoint_interval,"
               "cooldown_ms,control_seconds,treatment_seconds,"
               "control_scenarios_per_second,treatment_scenarios_per_second,"
               "pairwise_throughput_loss_percent,median_loss_percent,"
               "loss_q1_percent,loss_q3_percent,loss_iqr_percentage_points,"
               "loss_mad_percentage_points,confidence_95_low_percent,"
               "confidence_95_high_percent,control_drift_percent,"
               "order_effect_percentage_points,minimum_pairs,"
               "maximum_loss_percent,maximum_control_drift_percent,"
               "maximum_order_effect_percentage_points,sufficient_pairs,"
               "confidence_gate_pass,control_drift_gate_pass,"
               "order_effect_gate_pass,gate_pass,bootstrap_samples,"
               "bootstrap_seed,build_fingerprint,price\n";
        std::cout << std::fixed << std::setprecision(9);
        for (const Trial& trial : trials) {
            const mc::tool::CheckpointGateObservation observation{
                trial.control.scenarios_per_second,
                trial.treatment.scenarios_per_second,
                trial.control_first};
            std::cout
                << mc::tool::kCheckpointGateSchemaVersion << ','
                << trial.pair << ','
                << (trial.control_first ? "AB" : "BA") << ','
                << pairs << ',' << warmup_pairs << ','
                << spec.total_scenarios << ',' << spec.num_time_steps << ','
                << spec.global_seed << ',' << mc::to_string(spec.model_type)
                << ',' << mc::to_string(spec.payoff_type) << ',' << spec.spot
                << ',' << spec.strike << ',' << spec.rate << ','
                << spec.volatility << ',' << spec.maturity << ','
                << spec.engine_version << ',' << spec.rng_version << ','
                << spec.stats_schema_version << ','
                << engine_config.block_size << ',' << block_count << ','
                << engine_config.worker_count << ','
                << trial.control.workers_used << ',' << control_interval << ','
                << treatment_interval << ',' << cooldown_ms << ','
                << trial.control.seconds << ',' << trial.treatment.seconds
                << ',' << trial.control.scenarios_per_second << ','
                << trial.treatment.scenarios_per_second << ','
                << mc::tool::checkpoint_throughput_loss_percent(observation)
                << ','
                << summary.median_loss_percent << ','
                << summary.first_quartile_loss_percent << ','
                << summary.third_quartile_loss_percent << ','
                << summary.interquartile_range_percentage_points << ','
                << summary.median_absolute_deviation_percentage_points << ','
                << summary.confidence_95_low_percent << ','
                << summary.confidence_95_high_percent << ','
                << summary.control_drift_percent << ','
                << summary.order_effect_percentage_points << ','
                << policy.minimum_pairs << ',' << policy.maximum_loss_percent
                << ',' << policy.maximum_control_drift_percent << ','
                << policy.maximum_order_effect_percentage_points << ','
                << (summary.sufficient_pairs ? 1 : 0) << ','
                << (summary.confidence_gate_pass ? 1 : 0) << ','
                << (summary.control_drift_gate_pass ? 1 : 0) << ','
                << (summary.order_effect_gate_pass ? 1 : 0) << ','
                << (summary.gate_pass ? 1 : 0) << ','
                << policy.bootstrap_samples << ',' << policy.bootstrap_seed
                << ',' << mc::to_hex(build.hash) << ','
                << trial.control.aggregate.mean << '\n';
        }
        workspace.cleanup();
        if (require_pass && !summary.gate_pass) {
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
