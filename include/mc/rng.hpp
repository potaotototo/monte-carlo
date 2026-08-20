#pragma once

#include "mc/run_spec.hpp"

#include <array>
#include <cstdint>

namespace mc {

using PhiloxCounter = std::array<std::uint32_t, 4>;
using PhiloxKey = std::array<std::uint32_t, 2>;

// Random123 Philox4x32-10. The implementation is stateless: every output is a
// pure function of a 128-bit counter and a 64-bit key.
PhiloxCounter philox4x32_10(PhiloxCounter counter, PhiloxKey key) noexcept;

// Counter layout v1, from least to most significant logical field:
//   scenario_id: 40 bits, time_step: 24 bits,
//   dimension: 8 bits, draw_index: 24 bits, reserved: 32 zero bits.
PhiloxCounter pack_counter_v1(std::uint64_t scenario_id,
                              std::uint32_t time_step,
                              std::uint32_t dimension,
                              std::uint32_t draw_index);

// Raw 64-bit stream word formed from Philox output words 0 and 1. This is the
// exact source consumed by RNG v2's binary64 conversion and is exposed for
// known-answer tests and external statistical-battery adapters.
std::uint64_t random_u64(std::uint64_t global_seed,
                         std::uint64_t scenario_id,
                         std::uint32_t time_step,
                         std::uint32_t dimension = 0,
                         std::uint32_t draw_index = 0);

double uniform_open01(std::uint64_t global_seed,
                      std::uint64_t scenario_id,
                      std::uint32_t time_step,
                      std::uint32_t dimension = 0,
                      std::uint32_t draw_index = 0);

// RNG v2 conversion: retain the high 53 bits of two Philox words, scale by
// exactly 2^-53, and map the all-zero endpoint to the half-bin value 2^-54.
double uniform_from_words_v2(std::uint32_t high_word,
                             std::uint32_t low_word) noexcept;

// Acklam's stateless inverse-normal approximation. It consumes exactly one
// uniform value and never caches hidden state between logical draws.
double inverse_normal_cdf(double probability);

double standard_normal(std::uint64_t global_seed,
                       std::uint64_t scenario_id,
                       std::uint32_t time_step,
                       std::uint32_t dimension = 0,
                       std::uint32_t draw_index = 0);

}  // namespace mc
