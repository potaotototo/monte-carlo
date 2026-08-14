#include "mc/codec.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace mc {
namespace {

void append_u8(std::vector<std::uint8_t>& bytes, std::uint8_t value) {
    bytes.push_back(value);
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_double(std::vector<std::uint8_t>& bytes, double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    static_assert(std::numeric_limits<double>::is_iec559,
                  "canonical persistence requires IEEE-754 binary64 doubles");
    append_u64(bytes, std::bit_cast<std::uint64_t>(value));
}

void append_digest(std::vector<std::uint8_t>& bytes,
                   const Sha256Digest& digest) {
    bytes.insert(bytes.end(), digest.begin(), digest.end());
}

}  // namespace

std::vector<std::uint8_t> encode_run_spec_payload(const RunSpec& spec) {
    spec.validate();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(spec.model_type == ModelType::Heston ? 128U : 88U);
    append_u32(bytes, kRunSpecSchemaVersion);
    append_u64(bytes, spec.engine_version);
    append_u64(bytes, spec.rng_version);
    append_u32(bytes, spec.stats_schema_version);
    append_u8(bytes, static_cast<std::uint8_t>(spec.model_type));
    append_u8(bytes, static_cast<std::uint8_t>(spec.payoff_type));
    append_u8(bytes, spec.antithetic ? 1U : 0U);
    append_u8(bytes, 0U);  // Reserved in schema v1.
    append_u64(bytes, spec.global_seed);
    append_u64(bytes, spec.total_scenarios);
    append_u32(bytes, spec.num_time_steps);
    append_double(bytes, spec.maturity);
    append_double(bytes, spec.spot);
    append_double(bytes, spec.strike);
    append_double(bytes, spec.rate);
    // Schema v1 is a tagged payload: the common prefix is followed by exactly
    // one model-specific tail. Keeping the GBM tail byte-for-byte unchanged
    // preserves existing GBM run hashes and durable stores.
    switch (spec.model_type) {
        case ModelType::Gbm:
            append_double(bytes, spec.volatility);
            break;
        case ModelType::Heston:
            append_u32(bytes, spec.heston->discretization_version);
            append_double(bytes, spec.heston->initial_variance);
            append_double(bytes, spec.heston->mean_reversion_rate);
            append_double(bytes, spec.heston->long_run_variance);
            append_double(bytes, spec.heston->volatility_of_variance);
            append_double(bytes, spec.heston->correlation);
            break;
    }
    return bytes;
}

std::vector<std::uint8_t> encode_aggregate_payload(
    const AggregateStats& aggregate,
    std::uint32_t stats_schema_version) {
    if (stats_schema_version != kStatsSchemaVersion) {
        throw std::invalid_argument("unsupported aggregate statistics schema");
    }
    if (const std::optional<std::string> error = aggregate.invariant_error();
        error.has_value()) {
        throw std::invalid_argument(*error);
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(48U);
    append_u32(bytes, 1U);  // MeanVarianceV1 payload tag.
    append_u32(bytes, stats_schema_version);
    append_u64(bytes, aggregate.n);
    append_double(bytes, aggregate.mean);
    append_double(bytes, aggregate.m2);
    append_double(bytes, aggregate.min);
    append_double(bytes, aggregate.max);
    return bytes;
}

std::vector<std::uint8_t> encode_execution_layout_payload(
    const RunSpec& spec,
    const EngineConfig& config) {
    spec.validate();
    config.validate(spec);
    const std::uint64_t block_count =
        1U + (spec.total_scenarios - 1U) / config.block_size;
    if (block_count > config.max_materialized_blocks) {
        throw std::length_error(
            "execution layout exceeds max_materialized_blocks");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(56U);
    append_digest(bytes, run_spec_hash(spec));
    append_u32(bytes, kBlockPartitionVersion);
    append_u32(bytes, kReductionTreeVersion);
    append_u64(bytes, config.block_size);
    append_u64(bytes, block_count);
    return bytes;
}

Sha256Digest run_spec_hash(const RunSpec& spec) {
    return sha256(encode_run_spec_payload(spec));
}

Sha256Digest aggregate_payload_hash(const AggregateStats& aggregate,
                                    std::uint32_t stats_schema_version) {
    return sha256(encode_aggregate_payload(aggregate, stats_schema_version));
}

Sha256Digest execution_layout_hash(const RunSpec& spec,
                                   const EngineConfig& config) {
    return sha256(encode_execution_layout_payload(spec, config));
}

}  // namespace mc
