#include "checkpoint_gate_stats.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace mc::tool {
namespace {

double median(std::vector<double> values) {
    if (values.empty()) {
        throw std::invalid_argument("median requires at least one value");
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2U;
    return values.size() % 2U == 0U
               ? std::midpoint(values[middle - 1U], values[middle])
               : values[middle];
}

double quantile(std::vector<double> values, double probability) {
    if (values.empty() || !std::isfinite(probability) || probability < 0.0 ||
        probability > 1.0) {
        throw std::invalid_argument("quantile inputs are invalid");
    }
    std::sort(values.begin(), values.end());
    if (values.size() == 1U) {
        return values.front();
    }
    // Hyndman-Fan type 7, matching common spreadsheet/statistics defaults.
    const double position =
        probability * static_cast<double>(values.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return std::lerp(values[lower], values[upper], fraction);
}

std::uint64_t splitmix64(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

std::size_t sample_index(std::uint64_t& state, std::size_t count) {
    const std::uint64_t bound = static_cast<std::uint64_t>(count);
    const std::uint64_t rejection_threshold =
        static_cast<std::uint64_t>(-bound) % bound;
    for (;;) {
        const std::uint64_t value = splitmix64(state);
        if (value >= rejection_threshold) {
            return static_cast<std::size_t>(value % bound);
        }
    }
}

void validate_policy(const CheckpointGatePolicy& policy) {
    if (policy.minimum_pairs < 2U || policy.bootstrap_samples < 1'000U ||
        policy.bootstrap_samples > 1'000'000U ||
        !std::isfinite(policy.maximum_loss_percent) ||
        !std::isfinite(policy.maximum_control_drift_percent) ||
        !std::isfinite(policy.maximum_order_effect_percentage_points) ||
        policy.maximum_loss_percent < 0.0 ||
        policy.maximum_control_drift_percent < 0.0 ||
        policy.maximum_order_effect_percentage_points < 0.0) {
        throw std::invalid_argument("checkpoint gate policy is invalid");
    }
}

double robust_control_drift_percent(const std::vector<double>& rates) {
    std::vector<double> slopes;
    slopes.reserve(rates.size() * (rates.size() - 1U) / 2U);
    for (std::size_t left = 0U; left < rates.size(); ++left) {
        for (std::size_t right = left + 1U; right < rates.size(); ++right) {
            slopes.push_back(
                (rates[right] - rates[left]) /
                static_cast<double>(right - left));
        }
    }
    const double typical_rate = median(rates);
    return 100.0 * median(std::move(slopes)) *
           static_cast<double>(rates.size() - 1U) / typical_rate;
}

}  // namespace

double checkpoint_throughput_loss_percent(
    const CheckpointGateObservation& observation) {
    if (!std::isfinite(observation.control_scenarios_per_second) ||
        !std::isfinite(observation.treatment_scenarios_per_second) ||
        observation.control_scenarios_per_second <= 0.0 ||
        observation.treatment_scenarios_per_second <= 0.0) {
        throw std::invalid_argument(
            "checkpoint gate rates must be finite and positive");
    }
    const double loss =
        100.0 *
        (1.0 - observation.treatment_scenarios_per_second /
                   observation.control_scenarios_per_second);
    if (!std::isfinite(loss)) {
        throw std::invalid_argument(
            "checkpoint gate throughput loss is non-finite");
    }
    return loss;
}

CheckpointGateSummary summarize_checkpoint_gate(
    std::span<const CheckpointGateObservation> observations,
    const CheckpointGatePolicy& policy) {
    validate_policy(policy);
    if (observations.size() < 2U || observations.size() > 100U) {
        throw std::invalid_argument(
            "checkpoint gate requires 2 through 100 balanced observations");
    }

    std::vector<double> losses;
    std::vector<double> control_rates;
    std::vector<double> control_first_losses;
    std::vector<double> treatment_first_losses;
    losses.reserve(observations.size());
    control_rates.reserve(observations.size());
    for (const CheckpointGateObservation& observation : observations) {
        const double loss = checkpoint_throughput_loss_percent(observation);
        losses.push_back(loss);
        control_rates.push_back(observation.control_scenarios_per_second);
        (observation.control_first ? control_first_losses
                                   : treatment_first_losses)
            .push_back(loss);
    }
    if (control_first_losses.empty() || treatment_first_losses.empty() ||
        control_first_losses.size() > treatment_first_losses.size() + 1U ||
        treatment_first_losses.size() > control_first_losses.size() + 1U) {
        throw std::invalid_argument(
            "checkpoint gate observations are not balanced AB/BA pairs");
    }

    CheckpointGateSummary summary;
    summary.pairs = observations.size();
    summary.median_loss_percent = median(losses);
    summary.first_quartile_loss_percent = quantile(losses, 0.25);
    summary.third_quartile_loss_percent = quantile(losses, 0.75);
    summary.interquartile_range_percentage_points =
        summary.third_quartile_loss_percent -
        summary.first_quartile_loss_percent;
    std::vector<double> absolute_deviations;
    absolute_deviations.reserve(losses.size());
    for (const double loss : losses) {
        absolute_deviations.push_back(
            std::abs(loss - summary.median_loss_percent));
    }
    summary.median_absolute_deviation_percentage_points =
        median(std::move(absolute_deviations));

    std::vector<double> bootstrap_medians;
    bootstrap_medians.reserve(policy.bootstrap_samples);
    std::vector<double> resample(losses.size());
    std::uint64_t state = policy.bootstrap_seed;
    for (std::size_t sample = 0U; sample < policy.bootstrap_samples; ++sample) {
        for (double& value : resample) {
            value = losses[sample_index(state, losses.size())];
        }
        bootstrap_medians.push_back(median(resample));
    }
    summary.confidence_95_low_percent =
        quantile(bootstrap_medians, 0.025);
    summary.confidence_95_high_percent =
        quantile(std::move(bootstrap_medians), 0.975);
    summary.control_drift_percent =
        robust_control_drift_percent(control_rates);
    summary.order_effect_percentage_points =
        median(control_first_losses) - median(treatment_first_losses);
    if (!std::isfinite(summary.control_drift_percent) ||
        !std::isfinite(summary.order_effect_percentage_points)) {
        throw std::invalid_argument(
            "checkpoint gate diagnostics are non-finite");
    }

    summary.sufficient_pairs = observations.size() >= policy.minimum_pairs;
    summary.confidence_gate_pass =
        summary.confidence_95_high_percent < policy.maximum_loss_percent;
    summary.control_drift_gate_pass =
        std::abs(summary.control_drift_percent) <=
        policy.maximum_control_drift_percent;
    summary.order_effect_gate_pass =
        std::abs(summary.order_effect_percentage_points) <=
        policy.maximum_order_effect_percentage_points;
    summary.gate_pass = summary.sufficient_pairs &&
                        summary.confidence_gate_pass &&
                        summary.control_drift_gate_pass &&
                        summary.order_effect_gate_pass;
    return summary;
}

}  // namespace mc::tool
