#include "mc/coordinator.hpp"

#include "mc/codec.hpp"
#include "mc/identity.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mc {

CoordinatorState make_coordinator_state(const RunSpec& spec,
                                        const EngineConfig& config,
                                        std::vector<ScenarioBlock> blocks) {
    spec.validate();
    config.validate(spec);
    const std::uint64_t expected_block_count =
        1U + (spec.total_scenarios - 1U) / config.block_size;
    if (blocks.size() != expected_block_count) {
        throw std::invalid_argument("coordinator block universe has the wrong size");
    }
    if (blocks.empty()) {
        throw std::invalid_argument("coordinator block universe must not be empty");
    }

    const std::uint64_t incarnation = blocks.front().run_incarnation;
    std::uint64_t expected_start = 0;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const ScenarioBlock& block = blocks[index];
        const std::uint64_t expected_end =
            expected_start +
            std::min(config.block_size, spec.total_scenarios - expected_start);
        if (block.block_id != index || block.start_scenario != expected_start ||
            block.end_scenario != expected_end ||
            block.end_scenario <= block.start_scenario) {
            throw std::invalid_argument(
                "coordinator block universe is not contiguous and canonical");
        }
        if (block.run_incarnation != incarnation || block.lease_epoch == 0U) {
            throw std::invalid_argument(
                "coordinator block universe has inconsistent lease identity");
        }
        if (spec.antithetic &&
            (block.start_scenario % 2U != 0U || block.end_scenario % 2U != 0U)) {
            throw std::invalid_argument(
                "coordinator block universe splits an antithetic pair");
        }
        expected_start = expected_end;
    }
    if (expected_start != spec.total_scenarios) {
        throw std::invalid_argument(
            "coordinator block universe does not cover every scenario");
    }

    CoordinatorState state;
    state.run_spec_hash = run_spec_hash(spec);
    state.execution_layout_hash = mc::execution_layout_hash(spec, config);
    state.build_fingerprint = current_build_identity().hash;
    state.run_incarnation = incarnation;
    state.rng_version = spec.rng_version;
    state.stats_schema_version = spec.stats_schema_version;
    state.antithetic = spec.antithetic;
    state.blocks = std::move(blocks);
    state.committed_payloads.resize(state.blocks.size());
    return state;
}

Validation validate_result(const BlockResult& result,
                           const CoordinatorState& state) {
    if (result.run_spec_hash != state.run_spec_hash) {
        return {ValidationStatus::InvalidRun, "run specification hash mismatch"};
    }
    if (result.execution_layout_hash != state.execution_layout_hash) {
        return {ValidationStatus::ExecutionLayoutMismatch,
                "execution layout hash mismatch"};
    }
    if (result.build_fingerprint != state.build_fingerprint) {
        return {ValidationStatus::BuildMismatch, "build fingerprint mismatch"};
    }
    if (result.block.run_incarnation != state.run_incarnation) {
        return {ValidationStatus::StaleIncarnation, "run incarnation is stale"};
    }
    if (result.rng_version != state.rng_version) {
        return {ValidationStatus::RngVersionMismatch, "RNG version mismatch"};
    }
    if (result.stats_schema_version != state.stats_schema_version) {
        return {ValidationStatus::StatsSchemaMismatch,
                "statistics schema version mismatch"};
    }

    if (result.block.block_id >= state.blocks.size()) {
        return {ValidationStatus::InvalidBlock, "block ID is outside the run"};
    }
    const std::size_t block_index =
        static_cast<std::size_t>(result.block.block_id);
    const ScenarioBlock& expected = state.blocks[block_index];
    if (result.block.start_scenario != expected.start_scenario ||
        result.block.end_scenario != expected.end_scenario) {
        return {ValidationStatus::InvalidBlock, "block scenario range mismatch"};
    }
    const std::uint64_t scenario_count =
        expected.end_scenario - expected.start_scenario;
    const std::uint64_t expected_observations =
        state.antithetic ? scenario_count / 2U : scenario_count;
    if (const std::optional<std::string> error =
            result.aggregate.invariant_error(expected_observations);
        error.has_value()) {
        return {ValidationStatus::InvalidAggregate, *error};
    }

    const Sha256Digest computed_payload_hash =
        aggregate_payload_hash(result.aggregate, result.stats_schema_version);
    if (computed_payload_hash != result.payload_checksum) {
        return {ValidationStatus::CorruptPayload,
                "aggregate payload does not match its SHA-256 checksum"};
    }

    const std::optional<Sha256Digest>& committed =
        state.committed_payloads[block_index];
    if (committed.has_value()) {
        if (*committed == result.payload_checksum) {
            return {ValidationStatus::Duplicate,
                    "matching block payload is already committed"};
        }
        return {ValidationStatus::DeterminismError,
                "committed block has a conflicting payload checksum"};
    }

    if (result.block.lease_epoch != expected.lease_epoch) {
        return {ValidationStatus::StaleLease, "block lease epoch is stale"};
    }
    return {ValidationStatus::Accepted, "result is valid"};
}

Validation commit_result(const BlockResult& result, CoordinatorState& state) {
    const Validation validation = validate_result(result, state);
    if (validation.status == ValidationStatus::Accepted) {
        state.committed_payloads[static_cast<std::size_t>(result.block.block_id)] =
            result.payload_checksum;
    }
    return validation;
}

bool is_benign_rejection(ValidationStatus status) noexcept {
    return status == ValidationStatus::Duplicate ||
           status == ValidationStatus::StaleIncarnation ||
           status == ValidationStatus::StaleLease;
}

std::string to_string(ValidationStatus status) {
    switch (status) {
        case ValidationStatus::Accepted:
            return "accepted";
        case ValidationStatus::Duplicate:
            return "duplicate";
        case ValidationStatus::InvalidRun:
            return "invalid_run";
        case ValidationStatus::ExecutionLayoutMismatch:
            return "execution_layout_mismatch";
        case ValidationStatus::BuildMismatch:
            return "build_mismatch";
        case ValidationStatus::StaleIncarnation:
            return "stale_incarnation";
        case ValidationStatus::RngVersionMismatch:
            return "rng_version_mismatch";
        case ValidationStatus::StatsSchemaMismatch:
            return "stats_schema_mismatch";
        case ValidationStatus::InvalidAggregate:
            return "invalid_aggregate";
        case ValidationStatus::CorruptPayload:
            return "corrupt_payload";
        case ValidationStatus::InvalidBlock:
            return "invalid_block";
        case ValidationStatus::StaleLease:
            return "stale_lease";
        case ValidationStatus::DeterminismError:
            return "determinism_error";
    }
    throw std::invalid_argument("unknown validation status");
}

}  // namespace mc
