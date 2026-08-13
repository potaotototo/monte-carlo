#pragma once

#include "mc/run_spec.hpp"

#include <cstdint>

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

// Produces a discounted payoff. With antithetics enabled, scenario 2j+1 uses
// the negated stream of scenario 2j; pairing is handled by the block engine.
double simulate_discounted_payoff(const RunSpec& spec, std::uint64_t scenario_id);

double black_scholes_call_price(double spot,
                                double strike,
                                double rate,
                                double volatility,
                                double maturity);

}  // namespace mc
