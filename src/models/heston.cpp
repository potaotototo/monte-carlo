#include "mc/model.hpp"

#include "mc/rng.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace mc {

HestonKernel::HestonKernel(RunSpec spec) : spec_(std::move(spec)) {
    spec_.validate();
    if (spec_.model_type != ModelType::Heston) {
        throw std::invalid_argument(
            "Heston kernel requires a Heston run specification");
    }
    dt_ = spec_.maturity / static_cast<double>(spec_.num_time_steps);
    sqrt_dt_ = std::sqrt(dt_);
    rate_dt_ = spec_.rate * dt_;
    mean_reversion_dt_ = spec_.heston->mean_reversion_rate * dt_;
    volatility_of_variance_sqrt_dt_ =
        spec_.heston->volatility_of_variance * sqrt_dt_;
    const double rho = spec_.heston->correlation;
    orthogonal_weight_ = std::sqrt(std::max(0.0, 1.0 - rho * rho));
    discount_factor_ = std::exp(-spec_.rate * spec_.maturity);
    if (!std::isfinite(dt_) || dt_ <= 0.0 || !std::isfinite(sqrt_dt_) ||
        sqrt_dt_ <= 0.0 || !std::isfinite(rate_dt_) ||
        !std::isfinite(mean_reversion_dt_) ||
        !std::isfinite(volatility_of_variance_sqrt_dt_) ||
        !std::isfinite(mean_reversion_dt_ *
                       spec_.heston->long_run_variance) ||
        !std::isfinite(spec_.heston->initial_variance * dt_) ||
        !std::isfinite(std::sqrt(spec_.heston->initial_variance) * sqrt_dt_) ||
        !std::isfinite(spec_.heston->long_run_variance * dt_) ||
        !std::isfinite(orthogonal_weight_) ||
        !std::isfinite(discount_factor_) || discount_factor_ <= 0.0) {
        throw std::invalid_argument(
            "Heston parameters produce invalid derived constants");
    }
}

const RunSpec& HestonKernel::spec() const noexcept {
    return spec_;
}

double HestonKernel::payoff_from_path(double terminal_price,
                                      double running_sum) const {
    switch (spec_.payoff_type) {
        case PayoffType::EuropeanCall:
            return std::max(terminal_price - spec_.strike, 0.0);
        case PayoffType::AsianCall: {
            const double average =
                running_sum / static_cast<double>(spec_.num_time_steps);
            return std::max(average - spec_.strike, 0.0);
        }
    }
    throw std::logic_error("unreachable payoff type");
}

double HestonKernel::discounted_payoff_with_sign(
    std::uint64_t random_scenario_id,
    double shock_sign) const {
    const HestonParams& parameters = *spec_.heston;
    double price = spec_.spot;
    double variance = parameters.initial_variance;
    double running_sum = 0.0;
    for (std::uint32_t step = 0; step < spec_.num_time_steps; ++step) {
        // Dimensions 0 and 1 are independent logical streams. Correlation is
        // introduced explicitly, never by reusing or sequentially consuming a
        // mutable generator.
        const double z_asset = standard_normal(
            spec_.global_seed, random_scenario_id, step, 0U);
        const double z_orthogonal = standard_normal(
            spec_.global_seed, random_scenario_id, step, 1U);
        const double asset_shock = shock_sign * z_asset;
        const double variance_shock = shock_sign *
            (parameters.correlation * z_asset +
             orthogonal_weight_ * z_orthogonal);

        // Full-truncation Euler applies v+ in both drift and diffusion. The
        // stored state itself may be negative and is not silently clamped.
        const double positive_variance = std::max(variance, 0.0);
        const double sqrt_variance = std::sqrt(positive_variance);
        const double exponent =
            rate_dt_ - 0.5 * positive_variance * dt_ +
            sqrt_variance * sqrt_dt_ * asset_shock;
        const double next_price = price * std::exp(exponent);
        const double next_variance =
            variance + mean_reversion_dt_ *
                           (parameters.long_run_variance - positive_variance) +
            volatility_of_variance_sqrt_dt_ * sqrt_variance * variance_shock;
        if (!std::isfinite(exponent) || !std::isfinite(next_price) ||
            next_price <= 0.0 || !std::isfinite(next_variance)) {
            throw std::domain_error(
                "Heston path became numerically invalid at scenario " +
                std::to_string(random_scenario_id) + ", step " +
                std::to_string(step));
        }
        price = next_price;
        variance = next_variance;
        running_sum += price;
        if (!std::isfinite(running_sum)) {
            throw std::domain_error(
                "Heston running sum overflowed at scenario " +
                std::to_string(random_scenario_id) + ", step " +
                std::to_string(step));
        }
    }

    const double discounted =
        discount_factor_ * payoff_from_path(price, running_sum);
    if (!std::isfinite(discounted)) {
        throw std::domain_error("Heston simulation produced a non-finite payoff");
    }
    return discounted;
}

double HestonKernel::discounted_payoff(std::uint64_t scenario_id) const {
    if (scenario_id >= spec_.total_scenarios) {
        throw std::out_of_range("scenario_id is outside the run specification");
    }
    const std::uint64_t random_scenario =
        spec_.antithetic ? scenario_id & ~std::uint64_t{1} : scenario_id;
    const double sign =
        spec_.antithetic && scenario_id % 2U != 0U ? -1.0 : 1.0;
    return discounted_payoff_with_sign(random_scenario, sign);
}

double HestonKernel::antithetic_pair_mean(
    std::uint64_t even_scenario_id) const {
    if (!spec_.antithetic) {
        throw std::logic_error("antithetic pair requested for a plain run");
    }
    if (even_scenario_id % 2U != 0U ||
        even_scenario_id + 1U >= spec_.total_scenarios) {
        throw std::out_of_range(
            "antithetic pair must start at a valid even scenario");
    }
    const HestonParams& parameters = *spec_.heston;
    double positive_price = spec_.spot;
    double negative_price = spec_.spot;
    double positive_variance = parameters.initial_variance;
    double negative_variance = parameters.initial_variance;
    double positive_sum = 0.0;
    double negative_sum = 0.0;
    for (std::uint32_t step = 0; step < spec_.num_time_steps; ++step) {
        // Generate each independent normal once and advance both members of
        // the antithetic pair together. This halves RNG/inverse-CDF work versus
        // two separate path calls without changing scenario semantics.
        const double z_asset = standard_normal(
            spec_.global_seed, even_scenario_id, step, 0U);
        const double z_orthogonal = standard_normal(
            spec_.global_seed, even_scenario_id, step, 1U);
        const double correlated =
            parameters.correlation * z_asset +
            orthogonal_weight_ * z_orthogonal;

        const double positive_v_plus = std::max(positive_variance, 0.0);
        const double negative_v_plus = std::max(negative_variance, 0.0);
        const double positive_sqrt_v = std::sqrt(positive_v_plus);
        const double negative_sqrt_v = std::sqrt(negative_v_plus);
        const double positive_exponent =
            rate_dt_ - 0.5 * positive_v_plus * dt_ +
            positive_sqrt_v * sqrt_dt_ * z_asset;
        const double negative_exponent =
            rate_dt_ - 0.5 * negative_v_plus * dt_ -
            negative_sqrt_v * sqrt_dt_ * z_asset;
        const double next_positive_price =
            positive_price * std::exp(positive_exponent);
        const double next_negative_price =
            negative_price * std::exp(negative_exponent);
        const double next_positive_variance =
            positive_variance + mean_reversion_dt_ *
                                    (parameters.long_run_variance -
                                     positive_v_plus) +
            volatility_of_variance_sqrt_dt_ * positive_sqrt_v * correlated;
        const double next_negative_variance =
            negative_variance + mean_reversion_dt_ *
                                    (parameters.long_run_variance -
                                     negative_v_plus) -
            volatility_of_variance_sqrt_dt_ * negative_sqrt_v * correlated;
        if (!std::isfinite(positive_exponent) ||
            !std::isfinite(negative_exponent) ||
            !std::isfinite(next_positive_price) ||
            !std::isfinite(next_negative_price) ||
            next_positive_price <= 0.0 || next_negative_price <= 0.0 ||
            !std::isfinite(next_positive_variance) ||
            !std::isfinite(next_negative_variance)) {
            throw std::domain_error(
                "antithetic Heston path became numerically invalid at scenario " +
                std::to_string(even_scenario_id) + ", step " +
                std::to_string(step));
        }
        positive_price = next_positive_price;
        negative_price = next_negative_price;
        positive_variance = next_positive_variance;
        negative_variance = next_negative_variance;
        positive_sum += positive_price;
        negative_sum += negative_price;
        if (!std::isfinite(positive_sum) || !std::isfinite(negative_sum)) {
            throw std::domain_error(
                "antithetic Heston running sum overflowed at scenario " +
                std::to_string(even_scenario_id) + ", step " +
                std::to_string(step));
        }
    }

    const double positive = discount_factor_ *
        payoff_from_path(positive_price, positive_sum);
    const double negative = discount_factor_ *
        payoff_from_path(negative_price, negative_sum);
    const double pair_mean = 0.5 * (positive + negative);
    if (!std::isfinite(pair_mean)) {
        throw std::domain_error(
            "Heston simulation produced a non-finite pair mean");
    }
    return pair_mean;
}

ModelKernel::Kernel ModelKernel::make_kernel(RunSpec spec) {
    switch (spec.model_type) {
        case ModelType::Gbm:
            return GbmKernel(std::move(spec));
        case ModelType::Heston:
            return HestonKernel(std::move(spec));
    }
    throw std::invalid_argument("unsupported model type");
}

ModelKernel::ModelKernel(RunSpec spec) : kernel_(make_kernel(std::move(spec))) {}

const RunSpec& ModelKernel::spec() const noexcept {
    return std::visit([](const auto& kernel) -> const RunSpec& {
        return kernel.spec();
    }, kernel_);
}

double ModelKernel::discounted_payoff(std::uint64_t scenario_id) const {
    return std::visit([scenario_id](const auto& kernel) {
        return kernel.discounted_payoff(scenario_id);
    }, kernel_);
}

double ModelKernel::antithetic_pair_mean(
    std::uint64_t even_scenario_id) const {
    return std::visit([even_scenario_id](const auto& kernel) {
        return kernel.antithetic_pair_mean(even_scenario_id);
    }, kernel_);
}

}  // namespace mc
