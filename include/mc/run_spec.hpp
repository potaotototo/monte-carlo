#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mc {

inline constexpr std::uint64_t kEngineVersion = 2;
inline constexpr std::uint64_t kRngVersion = 2;
inline constexpr std::uint32_t kRunSpecSchemaVersion = 1;
inline constexpr std::uint32_t kStatsSchemaVersion = 1;
inline constexpr std::uint32_t kHestonDiscretizationVersion = 1;
inline constexpr std::uint64_t kMaxScenarios = std::uint64_t{1} << 40U;
inline constexpr std::uint32_t kMaxTimeSteps = std::uint32_t{1} << 24U;

enum class ModelType : std::uint8_t {
    Gbm = 1,
    Heston = 2,
};

enum class PayoffType : std::uint8_t {
    EuropeanCall = 1,
    AsianCall = 2,
};

struct HestonParams {
    double initial_variance = 0.04;
    double mean_reversion_rate = 1.5;
    double long_run_variance = 0.04;
    double volatility_of_variance = 0.3;
    double correlation = -0.7;
    std::uint32_t discretization_version = kHestonDiscretizationVersion;

    [[nodiscard]] double feller_ratio() const noexcept;
};

enum class RunWarningCode : std::uint8_t {
    HestonFellerConditionViolated = 1,
};

struct RunWarning {
    RunWarningCode code = RunWarningCode::HestonFellerConditionViolated;
    double observed_value = 0.0;
    double threshold = 1.0;
};

struct RunSpec {
    std::uint64_t global_seed = 1;
    std::uint64_t total_scenarios = 100'000;
    std::uint32_t num_time_steps = 1;
    double maturity = 1.0;
    double spot = 100.0;
    double strike = 100.0;
    double rate = 0.05;
    double volatility = 0.2;
    // Exactly one model parameter set is active. `volatility` belongs to GBM;
    // Heston uses this optional tagged payload instead.
    std::optional<HestonParams> heston;
    PayoffType payoff_type = PayoffType::EuropeanCall;
    bool antithetic = false;
    ModelType model_type = ModelType::Gbm;
    std::uint64_t engine_version = kEngineVersion;
    std::uint64_t rng_version = kRngVersion;
    std::uint32_t stats_schema_version = kStatsSchemaVersion;

    void validate() const;
    [[nodiscard]] std::vector<RunWarning> warnings() const;
};

std::string to_string(PayoffType payoff_type);
std::string to_string(ModelType model_type);
std::string to_string(RunWarningCode warning_code);

}  // namespace mc
