#include "mc/run_spec.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace mc {

double HestonParams::feller_ratio() const noexcept {
    if (volatility_of_variance == 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    if (mean_reversion_rate == 0.0 || long_run_variance == 0.0) {
        return 0.0;
    }
    if (!std::isfinite(mean_reversion_rate) ||
        !std::isfinite(long_run_variance) ||
        !std::isfinite(volatility_of_variance) ||
        mean_reversion_rate < 0.0 || long_run_variance < 0.0 ||
        volatility_of_variance < 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // Compose the ratio from normalized mantissas and base-two exponents.
    // Direct products can overflow on both sides of an otherwise finite
    // ratio; divide-first formulas can produce inf*0 and lose the warning.
    int mean_reversion_exponent = 0;
    int long_run_exponent = 0;
    int volatility_exponent = 0;
    const double mean_reversion_mantissa =
        std::frexp(mean_reversion_rate, &mean_reversion_exponent);
    const double long_run_mantissa =
        std::frexp(long_run_variance, &long_run_exponent);
    const double volatility_mantissa =
        std::frexp(volatility_of_variance, &volatility_exponent);
    const double mantissa =
        2.0 * mean_reversion_mantissa * long_run_mantissa /
        (volatility_mantissa * volatility_mantissa);
    const int exponent = mean_reversion_exponent + long_run_exponent -
                         2 * volatility_exponent;
    return std::scalbn(mantissa, exponent);
}

void RunSpec::validate() const {
    if (engine_version != kEngineVersion) {
        throw std::invalid_argument("unsupported engine version");
    }
    if (rng_version != kRngVersion) {
        throw std::invalid_argument("unsupported RNG version");
    }
    if (stats_schema_version != kStatsSchemaVersion) {
        throw std::invalid_argument("unsupported statistics schema version");
    }
    if (total_scenarios == 0 || total_scenarios > kMaxScenarios) {
        throw std::invalid_argument("total_scenarios must be in [1, 2^40]");
    }
    if (num_time_steps == 0 || num_time_steps > kMaxTimeSteps) {
        throw std::invalid_argument("num_time_steps must be in [1, 2^24]");
    }
    if (!std::isfinite(maturity) || maturity <= 0.0) {
        throw std::invalid_argument("maturity must be finite and positive");
    }
    if (!std::isfinite(spot) || spot <= 0.0) {
        throw std::invalid_argument("spot must be finite and positive");
    }
    if (!std::isfinite(strike) || strike <= 0.0) {
        throw std::invalid_argument("strike must be finite and positive");
    }
    if (!std::isfinite(rate)) {
        throw std::invalid_argument("rate must be finite");
    }
    if (antithetic && (total_scenarios % 2U != 0U)) {
        throw std::invalid_argument("antithetic runs require an even scenario count");
    }
    switch (payoff_type) {
        case PayoffType::EuropeanCall:
        case PayoffType::AsianCall:
            break;
        default:
            throw std::invalid_argument("unsupported payoff type");
    }
    switch (model_type) {
        case ModelType::Gbm:
            if (heston.has_value()) {
                throw std::invalid_argument(
                    "GBM runs must not contain Heston parameters");
            }
            if (!std::isfinite(volatility) || volatility <= 0.0) {
                throw std::invalid_argument(
                    "GBM volatility must be finite and positive");
            }
            break;
        case ModelType::Heston:
            if (!heston.has_value()) {
                throw std::invalid_argument(
                    "Heston runs require Heston parameters");
            }
            if (heston->discretization_version !=
                kHestonDiscretizationVersion) {
                throw std::invalid_argument(
                    "unsupported Heston discretization version");
            }
            if (!std::isfinite(heston->initial_variance) ||
                heston->initial_variance < 0.0) {
                throw std::invalid_argument(
                    "Heston initial variance must be finite and nonnegative");
            }
            if (!std::isfinite(heston->mean_reversion_rate) ||
                heston->mean_reversion_rate < 0.0) {
                throw std::invalid_argument(
                    "Heston mean reversion must be finite and nonnegative");
            }
            if (!std::isfinite(heston->long_run_variance) ||
                heston->long_run_variance < 0.0) {
                throw std::invalid_argument(
                    "Heston long-run variance must be finite and nonnegative");
            }
            if (!std::isfinite(heston->volatility_of_variance) ||
                heston->volatility_of_variance < 0.0) {
                throw std::invalid_argument(
                    "Heston volatility of variance must be finite and nonnegative");
            }
            if (!std::isfinite(heston->correlation) ||
                heston->correlation < -1.0 || heston->correlation > 1.0) {
                throw std::invalid_argument(
                    "Heston correlation must be finite and in [-1, 1]");
            }
            break;
        default:
            throw std::invalid_argument("unsupported model type");
    }
}

std::vector<RunWarning> RunSpec::warnings() const {
    validate();
    if (model_type != ModelType::Heston) {
        return {};
    }
    const double ratio = heston->feller_ratio();
    if (ratio < 1.0) {
        return {{RunWarningCode::HestonFellerConditionViolated, ratio, 1.0}};
    }
    return {};
}

std::string to_string(PayoffType payoff_type) {
    switch (payoff_type) {
        case PayoffType::EuropeanCall:
            return "european_call";
        case PayoffType::AsianCall:
            return "asian_call";
    }
    throw std::invalid_argument("unknown payoff type");
}

std::string to_string(ModelType model_type) {
    switch (model_type) {
        case ModelType::Gbm:
            return "gbm";
        case ModelType::Heston:
            return "heston";
    }
    throw std::invalid_argument("unknown model type");
}

std::string to_string(RunWarningCode warning_code) {
    switch (warning_code) {
        case RunWarningCode::HestonFellerConditionViolated:
            return "heston_feller_condition_violated";
    }
    throw std::invalid_argument("unknown run warning code");
}

}  // namespace mc
