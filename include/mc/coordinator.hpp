#pragma once

#include "mc/engine.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mc {

enum class ValidationStatus : std::uint8_t {
    Accepted,
    Duplicate,
    InvalidRun,
    ExecutionLayoutMismatch,
    BuildMismatch,
    StaleIncarnation,
    RngVersionMismatch,
    StatsSchemaMismatch,
    InvalidAggregate,
    CorruptPayload,
    InvalidBlock,
    StaleLease,
    DeterminismError,
};

struct Validation {
    ValidationStatus status = ValidationStatus::Accepted;
    std::string reason;
};

struct CoordinatorState {
    Sha256Digest run_spec_hash{};
    Sha256Digest execution_layout_hash{};
    Sha256Digest build_fingerprint{};
    std::uint64_t run_incarnation = 0;
    std::uint64_t rng_version = kRngVersion;
    std::uint32_t stats_schema_version = kStatsSchemaVersion;
    bool antithetic = false;
    std::vector<ScenarioBlock> blocks;
    std::vector<std::optional<Sha256Digest>> committed_payloads;
};

CoordinatorState make_coordinator_state(const RunSpec& spec,
                                        const EngineConfig& config,
                                        std::vector<ScenarioBlock> blocks);

// Side-effect-free so the durable coordinator can classify a result before it
// writes evidence or installs a manifest.
Validation validate_result(const BlockResult& result,
                           const CoordinatorState& state);

Validation commit_result(const BlockResult& result, CoordinatorState& state);
[[nodiscard]] bool is_benign_rejection(ValidationStatus status) noexcept;
std::string to_string(ValidationStatus status);

}  // namespace mc
