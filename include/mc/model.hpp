#pragma once

#include "mc/run_spec.hpp"

#include <cstdint>
#include <utility>
#include <variant>

namespace mc {

class GbmKernel {
public:
    explicit GbmKernel(RunSpec spec);

    [[nodiscard]] const RunSpec& spec() const noexcept;
    [[nodiscard]] double discounted_payoff(std::uint64_t scenario_id) const;
    [[nodiscard]] double antithetic_pair_mean(std::uint64_t even_scenario_id) const;

private:
    [[nodiscard]] double payoff_from_path(double terminal_price,
                                          double running_sum) const;

    RunSpec spec_;
    double drift_per_step_ = 0.0;
    double diffusion_per_step_ = 0.0;
    double discount_factor_ = 0.0;
};

class HestonKernel {
public:
    explicit HestonKernel(RunSpec spec);

    [[nodiscard]] const RunSpec& spec() const noexcept;
    [[nodiscard]] double discounted_payoff(std::uint64_t scenario_id) const;
    [[nodiscard]] double antithetic_pair_mean(std::uint64_t even_scenario_id) const;

private:
    [[nodiscard]] double payoff_from_path(double terminal_price,
                                          double running_sum) const;
    [[nodiscard]] double discounted_payoff_with_sign(
        std::uint64_t random_scenario_id,
        double shock_sign) const;

    RunSpec spec_;
    double dt_ = 0.0;
    double sqrt_dt_ = 0.0;
    double rate_dt_ = 0.0;
    double mean_reversion_dt_ = 0.0;
    double volatility_of_variance_sqrt_dt_ = 0.0;
    double orthogonal_weight_ = 0.0;
    double discount_factor_ = 0.0;
};

// Worker-local tagged dispatch. The model is selected once when a worker is
// created; there is no virtual call or allocation in the per-scenario loop.
class ModelKernel {
public:
    explicit ModelKernel(RunSpec spec);

    [[nodiscard]] const RunSpec& spec() const noexcept;
    [[nodiscard]] double discounted_payoff(std::uint64_t scenario_id) const;
    [[nodiscard]] double antithetic_pair_mean(std::uint64_t even_scenario_id) const;

    template <typename Visitor>
    decltype(auto) visit(Visitor&& visitor) const {
        return std::visit(std::forward<Visitor>(visitor), kernel_);
    }

private:
    using Kernel = std::variant<GbmKernel, HestonKernel>;
    static Kernel make_kernel(RunSpec spec);

    Kernel kernel_;
};

// Produces a discounted payoff. With antithetics enabled, scenario 2j+1 uses
// the negated stream of scenario 2j; pairing is handled by the block engine.
double simulate_discounted_payoff(const RunSpec& spec, std::uint64_t scenario_id);

double black_scholes_call_price(double spot,
                                double strike,
                                double rate,
                                double volatility,
                                double maturity);

}  // namespace mc
