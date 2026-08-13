#include "mc/rng.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace mc {
namespace {

constexpr std::uint32_t kPhiloxMultiplier0 = 0xD2511F53U;
constexpr std::uint32_t kPhiloxMultiplier1 = 0xCD9E8D57U;
constexpr std::uint32_t kPhiloxWeyl0 = 0x9E3779B9U;
constexpr std::uint32_t kPhiloxWeyl1 = 0xBB67AE85U;

struct Product32 {
    std::uint32_t low;
    std::uint32_t high;
};

Product32 multiply_high_low(std::uint32_t left, std::uint32_t right) noexcept {
    const auto product = static_cast<std::uint64_t>(left) *
                         static_cast<std::uint64_t>(right);
    return {
        static_cast<std::uint32_t>(product),
        static_cast<std::uint32_t>(product >> 32U),
    };
}

PhiloxCounter philox_round(const PhiloxCounter& counter,
                           const PhiloxKey& key) noexcept {
    const Product32 product0 = multiply_high_low(kPhiloxMultiplier0, counter[0]);
    const Product32 product1 = multiply_high_low(kPhiloxMultiplier1, counter[2]);
    return {
        product1.high ^ counter[1] ^ key[0],
        product1.low,
        product0.high ^ counter[3] ^ key[1],
        product0.low,
    };
}

}  // namespace

PhiloxCounter philox4x32_10(PhiloxCounter counter, PhiloxKey key) noexcept {
    for (int round = 0; round < 10; ++round) {
        counter = philox_round(counter, key);
        key[0] += kPhiloxWeyl0;
        key[1] += kPhiloxWeyl1;
    }
    return counter;
}

PhiloxCounter pack_counter_v1(std::uint64_t scenario_id,
                              std::uint32_t time_step,
                              std::uint32_t dimension,
                              std::uint32_t draw_index) {
    constexpr std::uint32_t kMax24BitValue = (std::uint32_t{1} << 24U) - 1U;
    if (scenario_id >= kMaxScenarios) {
        throw std::out_of_range("scenario_id exceeds RNG layout v1's 40-bit field");
    }
    if (time_step > kMax24BitValue) {
        throw std::out_of_range("time_step exceeds RNG layout v1's 24-bit field");
    }
    if (dimension > 0xFFU) {
        throw std::out_of_range("dimension exceeds RNG layout v1's 8-bit field");
    }
    if (draw_index > kMax24BitValue) {
        throw std::out_of_range("draw_index exceeds RNG layout v1's 24-bit field");
    }

    return {
        static_cast<std::uint32_t>(scenario_id),
        static_cast<std::uint32_t>((scenario_id >> 32U) |
                                   (static_cast<std::uint64_t>(time_step) << 8U)),
        dimension | (draw_index << 8U),
        0U,
    };
}

double uniform_open01(std::uint64_t global_seed,
                      std::uint64_t scenario_id,
                      std::uint32_t time_step,
                      std::uint32_t dimension,
                      std::uint32_t draw_index) {
    const PhiloxCounter counter =
        pack_counter_v1(scenario_id, time_step, dimension, draw_index);
    const PhiloxKey key = {
        static_cast<std::uint32_t>(global_seed),
        static_cast<std::uint32_t>(global_seed >> 32U),
    };
    const PhiloxCounter random_words = philox4x32_10(counter, key);
    return uniform_from_words_v2(random_words[0], random_words[1]);
}

double uniform_from_words_v2(std::uint32_t high_word,
                             std::uint32_t low_word) noexcept {
    const std::uint64_t raw =
        (static_cast<std::uint64_t>(high_word) << 32U) |
        static_cast<std::uint64_t>(low_word);
    const std::uint64_t retained = raw >> 11U;
    if (retained == 0U) {
        return 0x1.0p-54;
    }
    return static_cast<double>(retained) * 0x1.0p-53;
}

double inverse_normal_cdf(double probability) {
    if (!(probability > 0.0 && probability < 1.0)) {
        throw std::domain_error("inverse_normal_cdf requires probability in (0, 1)");
    }

    constexpr double a1 = -3.969683028665376e+01;
    constexpr double a2 = 2.209460984245205e+02;
    constexpr double a3 = -2.759285104469687e+02;
    constexpr double a4 = 1.383577518672690e+02;
    constexpr double a5 = -3.066479806614716e+01;
    constexpr double a6 = 2.506628277459239e+00;

    constexpr double b1 = -5.447609879822406e+01;
    constexpr double b2 = 1.615858368580409e+02;
    constexpr double b3 = -1.556989798598866e+02;
    constexpr double b4 = 6.680131188771972e+01;
    constexpr double b5 = -1.328068155288572e+01;

    constexpr double c1 = -7.784894002430293e-03;
    constexpr double c2 = -3.223964580411365e-01;
    constexpr double c3 = -2.400758277161838e+00;
    constexpr double c4 = -2.549732539343734e+00;
    constexpr double c5 = 4.374664141464968e+00;
    constexpr double c6 = 2.938163982698783e+00;

    constexpr double d1 = 7.784695709041462e-03;
    constexpr double d2 = 3.224671290700398e-01;
    constexpr double d3 = 2.445134137142996e+00;
    constexpr double d4 = 3.754408661907416e+00;

    constexpr double low_tail = 0.02425;
    constexpr double high_tail = 1.0 - low_tail;

    if (probability < low_tail) {
        const double q = std::sqrt(-2.0 * std::log(probability));
        return (((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
               ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
    }
    if (probability <= high_tail) {
        const double q = probability - 0.5;
        const double r = q * q;
        return (((((a1 * r + a2) * r + a3) * r + a4) * r + a5) * r + a6) * q /
               (((((b1 * r + b2) * r + b3) * r + b4) * r + b5) * r + 1.0);
    }

    const double q = std::sqrt(-2.0 * std::log(1.0 - probability));
    return -(((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
           ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
}

double standard_normal(std::uint64_t global_seed,
                       std::uint64_t scenario_id,
                       std::uint32_t time_step,
                       std::uint32_t dimension,
                       std::uint32_t draw_index) {
    return inverse_normal_cdf(uniform_open01(global_seed,
                                             scenario_id,
                                             time_step,
                                             dimension,
                                             draw_index));
}

}  // namespace mc
