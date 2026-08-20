#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace mc::tool {

inline constexpr std::uint32_t kCheckpointGateSchemaVersion = 1U;

struct CheckpointGateObservation {
    double control_scenarios_per_second = 0.0;
    double treatment_scenarios_per_second = 0.0;
    bool control_first = true;
};

struct CheckpointGatePolicy {
    std::size_t minimum_pairs = 20U;
    std::size_t bootstrap_samples = 10'000U;
    // ASCII "R4CHKPT"; resampling is reproducible from retained raw rows.
    std::uint64_t bootstrap_seed = 0x523443484B5054ULL;
    double maximum_loss_percent = 10.0;
    double maximum_control_drift_percent = 5.0;
    double maximum_order_effect_percentage_points = 5.0;
};

struct CheckpointGateSummary {
    std::size_t pairs = 0U;
    double median_loss_percent = 0.0;
    double first_quartile_loss_percent = 0.0;
    double third_quartile_loss_percent = 0.0;
    double interquartile_range_percentage_points = 0.0;
    double median_absolute_deviation_percentage_points = 0.0;
    double confidence_95_low_percent = 0.0;
    double confidence_95_high_percent = 0.0;
    double control_drift_percent = 0.0;
    double order_effect_percentage_points = 0.0;
    bool sufficient_pairs = false;
    bool confidence_gate_pass = false;
    bool control_drift_gate_pass = false;
    bool order_effect_gate_pass = false;
    bool gate_pass = false;
};

[[nodiscard]] double checkpoint_throughput_loss_percent(
    const CheckpointGateObservation& observation);

[[nodiscard]] CheckpointGateSummary summarize_checkpoint_gate(
    std::span<const CheckpointGateObservation> observations,
    const CheckpointGatePolicy& policy = {});

}  // namespace mc::tool
