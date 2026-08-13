#pragma once

#include "mc/aggregate.hpp"
#include "mc/engine.hpp"
#include "mc/hash.hpp"
#include "mc/run_spec.hpp"

#include <cstdint>
#include <vector>

namespace mc {

// These payload encoders are the canonical, checksummed representation used by
// result validation now and by the R2 on-disk envelopes later. Fields are fixed
// order and little-endian; doubles use their IEEE-754 binary64 bit patterns.
std::vector<std::uint8_t> encode_run_spec_payload(const RunSpec& spec);
std::vector<std::uint8_t> encode_aggregate_payload(const AggregateStats& aggregate,
                                                   std::uint32_t stats_schema_version);
std::vector<std::uint8_t> encode_execution_layout_payload(
    const RunSpec& spec,
    const EngineConfig& config);

Sha256Digest run_spec_hash(const RunSpec& spec);
Sha256Digest aggregate_payload_hash(const AggregateStats& aggregate,
                                    std::uint32_t stats_schema_version);
Sha256Digest execution_layout_hash(const RunSpec& spec,
                                   const EngineConfig& config);

}  // namespace mc
