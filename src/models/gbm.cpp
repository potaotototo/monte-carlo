#include "mc/model.hpp"

#include "mc/rng.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace mc {
namespace {

double normal_cdf(double value) {
    return 0.5 * std::erfc(-value / std::sqrt(2.0));
}

}  // namespace

GbmKernel::GbmKernel(RunSpec spec) : spec_(std::move(spec)) {
    spec_.validate();
    if (spec_.model_type != ModelType::Gbm) {
        throw std::invalid_argument("GBM kernel requires a GBM run specification");
    }
    const double dt = spec_.maturity / static_cast<double>(spec_.num_time_steps);
    drift_per_step_ =
        (spec_.rate - 0.5 * spec_.volatility * spec_.volatility) * dt;
    diffusion_per_step_ = spec_.volatility * std::sqrt(dt);
    discount_factor_ = std::exp(-spec_.rate * spec_.maturity);
    if (!std::isfinite(dt) || !std::isfinite(drift_per_step_) ||
        !std::isfinite(diffusion_per_step_) ||
        !std::isfinite(discount_factor_) || discount_factor_ <= 0.0) {
        throw std::invalid_argument(
            "GBM parameters produce non-finite or underflowed derived constants");
    }
}

const RunSpec& GbmKernel::spec() const noexcept {
    return spec_;
}

double GbmKernel::payoff_from_path(double terminal_price,
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

double GbmKernel::discounted_payoff(std::uint64_t scenario_id) const {
    if (scenario_id >= spec_.total_scenarios) {
        throw std::out_of_range("scenario_id is outside the run specification");
    }

    double price = spec_.spot;
    double running_sum = 0.0;
    for (std::uint32_t step = 0; step < spec_.num_time_steps; ++step) {
        const std::uint64_t random_scenario =
            spec_.antithetic ? scenario_id & ~std::uint64_t{1} : scenario_id;
        double normal =
            standard_normal(spec_.global_seed, random_scenario, step);
        if (spec_.antithetic && scenario_id % 2U != 0U) {
            normal = -normal;
        }
        const double exponent = drift_per_step_ + diffusion_per_step_ * normal;
        const double next_price = price * std::exp(exponent);
        if (!std::isfinite(exponent) || !std::isfinite(next_price) ||
            next_price <= 0.0) {
            throw std::domain_error(
                "GBM path became numerically invalid at scenario " +
                std::to_string(scenario_id) + ", step " + std::to_string(step));
        }
        price = next_price;
        running_sum += price;
        if (!std::isfinite(running_sum)) {
            throw std::domain_error(
                "GBM running sum overflowed at scenario " +
                std::to_string(scenario_id) + ", step " + std::to_string(step));
        }
    }

    const double discounted = discount_factor_ * payoff_from_path(price, running_sum);
    if (!std::isfinite(discounted)) {
        throw std::domain_error("GBM simulation produced a non-finite payoff");
    }
    return discounted;
}

double GbmKernel::antithetic_pair_mean(std::uint64_t even_scenario_id) const {
    if (!spec_.antithetic) {
        throw std::logic_error("antithetic pair requested for a plain run");
    }
    if (even_scenario_id % 2U != 0U ||
        even_scenario_id + 1U >= spec_.total_scenarios) {
        throw std::out_of_range("antithetic pair must start at a valid even scenario");
    }

    double positive_price = spec_.spot;
    double negative_price = spec_.spot;
    double positive_sum = 0.0;
    double negative_sum = 0.0;
    for (std::uint32_t step = 0; step < spec_.num_time_steps; ++step) {
        const double normal =
            standard_normal(spec_.global_seed, even_scenario_id, step);
        const double positive_exponent =
            drift_per_step_ + diffusion_per_step_ * normal;
        const double negative_exponent =
            drift_per_step_ + diffusion_per_step_ * -normal;
        const double next_positive = positive_price * std::exp(positive_exponent);
        const double next_negative = negative_price * std::exp(negative_exponent);
        if (!std::isfinite(positive_exponent) ||
            !std::isfinite(negative_exponent) || !std::isfinite(next_positive) ||
            !std::isfinite(next_negative) || next_positive <= 0.0 ||
            next_negative <= 0.0) {
            throw std::domain_error(
                "antithetic GBM path became numerically invalid at scenario " +
                std::to_string(even_scenario_id) + ", step " +
                std::to_string(step));
        }
        positive_price = next_positive;
        negative_price = next_negative;
        positive_sum += positive_price;
        negative_sum += negative_price;
        if (!std::isfinite(positive_sum) || !std::isfinite(negative_sum)) {
            throw std::domain_error(
                "antithetic GBM running sum overflowed at scenario " +
                std::to_string(even_scenario_id) + ", step " +
                std::to_string(step));
        }
    }

    const double positive =
        discount_factor_ * payoff_from_path(positive_price, positive_sum);
    const double negative =
        discount_factor_ * payoff_from_path(negative_price, negative_sum);
    const double pair_mean = 0.5 * (positive + negative);
    if (!std::isfinite(pair_mean)) {
        throw std::domain_error("GBM simulation produced a non-finite pair mean");
    }
    return pair_mean;
}

double simulate_discounted_payoff(const RunSpec& spec, std::uint64_t scenario_id) {
    return GbmKernel(spec).discounted_payoff(scenario_id);
}

double black_scholes_call_price(double spot,
                                double strike,
                                double rate,
                                double volatility,
                                double maturity) {
    if (!std::isfinite(spot) || !std::isfinite(strike) ||
        !std::isfinite(rate) || !std::isfinite(volatility) ||
        !std::isfinite(maturity) ||
        !(spot > 0.0 && strike > 0.0 && volatility > 0.0 && maturity > 0.0)) {
        throw std::invalid_argument(
            "Black-Scholes inputs must be finite and required values positive");
    }
    const double scaled_volatility = volatility * std::sqrt(maturity);
    const double d1 = (std::log(spot / strike) +
                       (rate + 0.5 * volatility * volatility) * maturity) /
                      scaled_volatility;
    const double d2 = d1 - scaled_volatility;
    const double result = spot * normal_cdf(d1) -
                          strike * std::exp(-rate * maturity) * normal_cdf(d2);
    if (!std::isfinite(result)) {
        throw std::domain_error("Black-Scholes calculation produced a non-finite price");
    }
    return result;
}

}  // namespace mc
