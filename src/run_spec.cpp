#include "mc/run_spec.hpp"

#include <cmath>
#include <stdexcept>

namespace mc {

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
    if (!std::isfinite(volatility) || volatility <= 0.0) {
        throw std::invalid_argument("volatility must be finite and positive");
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
            break;
        default:
            throw std::invalid_argument("unsupported model type");
    }
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
    }
    throw std::invalid_argument("unknown model type");
}

}  // namespace mc
