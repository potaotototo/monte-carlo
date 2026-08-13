#pragma once

#include "mc/aggregate.hpp"
#include "mc/hash.hpp"
#include "mc/run_spec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mc {

inline constexpr std::uint64_t kMinNormalConfidenceObservations = 30;
inline constexpr std::uint32_t kBlockPartitionVersion = 1;
inline constexpr std::uint32_t kReductionTreeVersion = 1;
inline constexpr std::size_t kMaxWorkerThreads = 256;
inline constexpr std::uint64_t kDefaultMaxMaterializedBlocks = 1'000'000;

struct ScenarioBlock {
    std::uint64_t block_id = 0;
    std::uint64_t start_scenario = 0;
    std::uint64_t end_scenario = 0;
    std::uint64_t run_incarnation = 0;
    std::uint64_t lease_epoch = 1;
};

struct BlockResult {
    ScenarioBlock block;
    AggregateStats aggregate;
    Sha256Digest run_spec_hash{};
    Sha256Digest execution_layout_hash{};
    Sha256Digest build_fingerprint{};
    Sha256Digest payload_checksum{};
    std::uint64_t rng_version = kRngVersion;
    std::uint32_t stats_schema_version = kStatsSchemaVersion;
    std::uint64_t worker_id = 0;
};

struct EngineConfig {
    std::size_t worker_count = 1;
    std::uint64_t block_size = 2'048;
    std::size_t assignment_queue_capacity = 0;
    std::size_t completion_queue_capacity = 0;
    std::uint64_t max_materialized_blocks = kDefaultMaxMaterializedBlocks;

    void validate(const RunSpec& spec) const;
};

struct RunResult {
    AggregateStats aggregate;
    std::uint64_t scenarios_processed = 0;
    std::uint64_t block_count = 0;
    std::size_t workers_used = 0;

    [[nodiscard]] std::optional<double> confidence_low(double z = 1.96) const;
    [[nodiscard]] std::optional<double> confidence_high(double z = 1.96) const;
};

std::vector<ScenarioBlock> make_blocks(const RunSpec& spec,
                                       const EngineConfig& config);

AggregateStats compute_block(const RunSpec& spec, const ScenarioBlock& block);

// Fixed-position pairwise reduction over block_id. Completion order never
// changes the tree shape.
AggregateStats reduce_block_results(const std::vector<AggregateStats>& leaves);

RunResult run_parallel(const RunSpec& spec, const EngineConfig& config);

}  // namespace mc
