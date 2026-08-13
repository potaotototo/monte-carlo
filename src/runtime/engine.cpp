#include "mc/engine.hpp"

#include "mc/bounded_queue.hpp"
#include "mc/codec.hpp"
#include "mc/coordinator.hpp"
#include "mc/model.hpp"
#include "mc/persistence.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace mc {
namespace {

AggregateStats compute_block_with_kernel(const GbmKernel& kernel,
                                         const ScenarioBlock& block) {
    const RunSpec& spec = kernel.spec();
    if (block.start_scenario >= block.end_scenario ||
        block.end_scenario > spec.total_scenarios) {
        throw std::invalid_argument("invalid scenario block range");
    }
    if (spec.antithetic &&
        ((block.start_scenario % 2U != 0U) || (block.end_scenario % 2U != 0U))) {
        throw std::invalid_argument("antithetic block boundaries must be even");
    }

    AggregateStats aggregate;
    if (spec.antithetic) {
        for (std::uint64_t scenario = block.start_scenario;
             scenario < block.end_scenario;
             scenario += 2U) {
            aggregate.add(kernel.antithetic_pair_mean(scenario));
        }
        return aggregate;
    }

    for (std::uint64_t scenario = block.start_scenario;
         scenario < block.end_scenario;
         ++scenario) {
        aggregate.add(kernel.discounted_payoff(scenario));
    }
    return aggregate;
}

std::size_t execute_missing_blocks(
    const RunSpec& spec,
    const EngineConfig& config,
    const std::vector<ScenarioBlock>& blocks,
    const std::vector<bool>& already_received,
    const Sha256Digest& spec_hash,
    const Sha256Digest& layout_hash,
    const Sha256Digest& build_fingerprint,
    const std::function<void(BlockResult&&)>& consume_result) {
    if (already_received.size() != blocks.size()) {
        throw std::invalid_argument("received bitmap does not match block universe");
    }
    const std::size_t missing_count = static_cast<std::size_t>(std::count(
        already_received.begin(), already_received.end(), false));
    std::vector<std::size_t> pending_indices;
    pending_indices.reserve(missing_count);
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        if (!already_received[index]) {
            pending_indices.push_back(index);
        }
    }
    if (missing_count == 0U) {
        return 0U;
    }
    const std::size_t actual_worker_count =
        std::min(config.worker_count, missing_count);
    const std::size_t assignment_capacity =
        config.assignment_queue_capacity == 0
            ? std::max<std::size_t>(1, actual_worker_count * 2U)
            : config.assignment_queue_capacity;
    const std::size_t completion_capacity =
        config.completion_queue_capacity == 0
            ? std::max<std::size_t>(1, actual_worker_count * 2U)
            : config.completion_queue_capacity;

    BoundedQueue<ScenarioBlock> assignments(assignment_capacity);
    BoundedQueue<BlockResult> completions(completion_capacity);
    std::mutex error_mutex;
    std::exception_ptr first_error;

    const auto record_error = [&](std::exception_ptr error) {
        {
            std::lock_guard lock(error_mutex);
            if (!first_error) {
                first_error = std::move(error);
            }
        }
        assignments.close();
        completions.close();
    };

    std::atomic<std::size_t> workers_remaining{actual_worker_count};
    std::vector<std::thread> workers;
    workers.reserve(actual_worker_count);
    try {
        for (std::size_t worker_index = 0;
             worker_index < actual_worker_count;
             ++worker_index) {
            workers.emplace_back([&, worker_index] {
                try {
                    const GbmKernel kernel(spec);
                    ScenarioBlock block;
                    while (assignments.pop(block)) {
                        BlockResult result;
                        result.block = block;
                        result.aggregate = compute_block_with_kernel(kernel, block);
                        result.run_spec_hash = spec_hash;
                        result.execution_layout_hash = layout_hash;
                        result.build_fingerprint = build_fingerprint;
                        result.payload_checksum = aggregate_payload_hash(
                            result.aggregate, spec.stats_schema_version);
                        result.rng_version = spec.rng_version;
                        result.stats_schema_version = spec.stats_schema_version;
                        result.worker_id =
                            static_cast<std::uint64_t>(worker_index);
                        if (!completions.push(std::move(result))) {
                            break;
                        }
                    }
                } catch (...) {
                    record_error(std::current_exception());
                }

                if (workers_remaining.fetch_sub(1U) == 1U) {
                    completions.close();
                }
            });
        }
    } catch (...) {
        assignments.close();
        completions.close();
        for (std::thread& worker : workers) {
            worker.join();
        }
        throw;
    }

    std::thread scheduler;
    try {
        scheduler = std::thread([&] {
            try {
                for (const std::size_t index : pending_indices) {
                    if (!assignments.push(blocks[index])) {
                        break;
                    }
                }
                assignments.close();
            } catch (...) {
                record_error(std::current_exception());
            }
        });
    } catch (...) {
        assignments.close();
        completions.close();
        for (std::thread& worker : workers) {
            worker.join();
        }
        throw;
    }

    BlockResult completed;
    while (completions.pop(completed)) {
        try {
            consume_result(std::move(completed));
        } catch (...) {
            record_error(std::current_exception());
            break;
        }
    }

    scheduler.join();
    for (std::thread& worker : workers) {
        worker.join();
    }
    if (first_error) {
        std::rethrow_exception(first_error);
    }
    return actual_worker_count;
}

}  // namespace

void EngineConfig::validate(const RunSpec& spec) const {
    if (worker_count == 0) {
        throw std::invalid_argument("worker_count must be positive");
    }
    if (worker_count > kMaxWorkerThreads) {
        throw std::invalid_argument("worker_count exceeds the safety limit of 256");
    }
    if (block_size == 0) {
        throw std::invalid_argument("block_size must be positive");
    }
    if (spec.antithetic && (block_size % 2U != 0U)) {
        throw std::invalid_argument("antithetic runs require an even block size");
    }
    if (max_materialized_blocks == 0U) {
        throw std::invalid_argument("max_materialized_blocks must be positive");
    }
}

std::optional<double> RunResult::confidence_low(double z) const {
    if (!std::isfinite(z) || z <= 0.0) {
        throw std::invalid_argument("confidence critical value must be finite and positive");
    }
    if (aggregate.n < kMinNormalConfidenceObservations) {
        return std::nullopt;
    }
    const std::optional<double> error = aggregate.standard_error();
    return error.has_value()
               ? std::optional<double>{aggregate.mean - z * *error}
               : std::nullopt;
}

std::optional<double> RunResult::confidence_high(double z) const {
    if (!std::isfinite(z) || z <= 0.0) {
        throw std::invalid_argument("confidence critical value must be finite and positive");
    }
    if (aggregate.n < kMinNormalConfidenceObservations) {
        return std::nullopt;
    }
    const std::optional<double> error = aggregate.standard_error();
    return error.has_value()
               ? std::optional<double>{aggregate.mean + z * *error}
               : std::nullopt;
}

std::vector<ScenarioBlock> make_blocks(const RunSpec& spec,
                                       const EngineConfig& config) {
    spec.validate();
    config.validate(spec);

    const std::uint64_t block_count =
        1U + (spec.total_scenarios - 1U) / config.block_size;
    if (block_count > config.max_materialized_blocks) {
        throw std::length_error(
            "run requires " + std::to_string(block_count) +
            " blocks, exceeding max_materialized_blocks=" +
            std::to_string(config.max_materialized_blocks));
    }
    std::vector<ScenarioBlock> blocks;
    blocks.reserve(static_cast<std::size_t>(block_count));

    for (std::uint64_t block_id = 0; block_id < block_count; ++block_id) {
        const std::uint64_t start = block_id * config.block_size;
        const std::uint64_t end =
            start + std::min(config.block_size, spec.total_scenarios - start);
        blocks.push_back(ScenarioBlock{block_id, start, end, 0, 1});
    }
    return blocks;
}

AggregateStats compute_block(const RunSpec& spec, const ScenarioBlock& block) {
    return compute_block_with_kernel(GbmKernel(spec), block);
}

AggregateStats reduce_block_results(const std::vector<AggregateStats>& leaves) {
    if (leaves.empty()) {
        return {};
    }

    std::vector<AggregateStats> level = leaves;
    while (level.size() > 1) {
        std::vector<AggregateStats> next;
        next.reserve((level.size() + 1U) / 2U);
        for (std::size_t index = 0; index < level.size(); index += 2U) {
            if (index + 1U < level.size()) {
                next.push_back(merge(level[index], level[index + 1U]));
            } else {
                next.push_back(level[index]);
            }
        }
        level = std::move(next);
    }
    return level.front();
}

RunResult run_parallel(const RunSpec& spec, const EngineConfig& config) {
    const std::vector<ScenarioBlock> blocks = make_blocks(spec, config);
    CoordinatorState coordinator = make_coordinator_state(spec, config, blocks);
    std::vector<AggregateStats> leaves(blocks.size());
    std::vector<bool> received(blocks.size(), false);
    const std::size_t actual_worker_count = execute_missing_blocks(
        spec, config, blocks, received, coordinator.run_spec_hash,
        coordinator.execution_layout_hash, coordinator.build_fingerprint,
        [&](BlockResult&& completed) {
            const Validation validation = commit_result(completed, coordinator);
            if (validation.status != ValidationStatus::Accepted) {
                throw std::runtime_error(
                    "coordinator rejected block result as " +
                    to_string(validation.status) + ": " + validation.reason);
            }
            const std::size_t leaf =
                static_cast<std::size_t>(completed.block.block_id);
            leaves[leaf] = completed.aggregate;
            received[leaf] = true;
        });
    if (std::find(received.begin(), received.end(), false) != received.end()) {
        throw std::runtime_error("parallel run ended before every block completed");
    }

    return RunResult{
        reduce_block_results(leaves),
        spec.total_scenarios,
        static_cast<std::uint64_t>(blocks.size()),
        actual_worker_count,
    };
}

DurableRunResult run_parallel_durable(
    const RunSpec& spec,
    const EngineConfig& engine_config,
    const RunStoreConfig& store_config) {
    DurableRunStore store =
        DurableRunStore::open(spec, engine_config, store_config);
    const DurableRecoveryState& recovery = store.recovery_state();
    if (recovery.status == DurableRunStatus::Failed) {
        throw std::runtime_error(
            "durable run is in a failed state and requires operator intervention");
    }

    CoordinatorState coordinator = make_coordinator_state(
        spec, engine_config, recovery.blocks);
    std::vector<AggregateStats> leaves(recovery.blocks.size());
    std::vector<bool> received(recovery.blocks.size(), false);
    std::uint64_t recovered_blocks = 0;
    std::uint64_t recovered_scenarios = 0;
    for (std::size_t index = 0;
         index < recovery.committed_results.size();
         ++index) {
        if (!recovery.committed_results[index].has_value()) {
            continue;
        }
        const BlockResult& result = *recovery.committed_results[index];
        coordinator.committed_payloads[index] = result.payload_checksum;
        leaves[index] = result.aggregate;
        received[index] = true;
        ++recovered_blocks;
        recovered_scenarios +=
            result.block.end_scenario - result.block.start_scenario;
    }

    std::uint64_t computed_blocks = 0;
    std::uint64_t computed_scenarios = 0;
    std::uint64_t pending_since_checkpoint = 0;
    std::size_t workers_used = 0;
    if (recovery.status != DurableRunStatus::Complete) {
        workers_used = execute_missing_blocks(
            spec, engine_config, recovery.blocks, received,
            coordinator.run_spec_hash, coordinator.execution_layout_hash,
            coordinator.build_fingerprint,
            [&](BlockResult&& completed) {
                const Validation validation =
                    validate_result(completed, coordinator);
                if (is_benign_rejection(validation.status)) {
                    return;
                }
                if (validation.status != ValidationStatus::Accepted) {
                    FailureRecord failure;
                    failure.status = validation.status;
                    failure.block_id = completed.block.block_id;
                    failure.run_incarnation =
                        completed.block.run_incarnation;
                    failure.lease_epoch = completed.block.lease_epoch;
                    failure.observed_checksum = completed.payload_checksum;
                    const std::size_t index = static_cast<std::size_t>(
                        std::min<std::uint64_t>(
                            completed.block.block_id,
                            coordinator.committed_payloads.size() - 1U));
                    if (coordinator.committed_payloads[index].has_value()) {
                        failure.committed_checksum =
                            *coordinator.committed_payloads[index];
                    }
                    failure.reason = validation.reason;
                    store.checkpoint(coordinator, leaves, received,
                                     DurableRunStatus::Failed, failure);
                    throw std::runtime_error(
                        "coordinator rejected durable block result as " +
                        to_string(validation.status) + ": " +
                        validation.reason);
                }

                store.record_result(completed);
                const Validation committed =
                    commit_result(completed, coordinator);
                if (committed.status != ValidationStatus::Accepted) {
                    throw std::logic_error(
                        "validated durable result changed classification");
                }
                const std::size_t leaf =
                    static_cast<std::size_t>(completed.block.block_id);
                leaves[leaf] = completed.aggregate;
                received[leaf] = true;
                ++computed_blocks;
                computed_scenarios +=
                    completed.block.end_scenario -
                    completed.block.start_scenario;
                ++pending_since_checkpoint;
                if (pending_since_checkpoint >=
                        store.config().checkpoint_interval_blocks &&
                    recovered_blocks + computed_blocks <
                        static_cast<std::uint64_t>(recovery.blocks.size())) {
                    store.checkpoint(coordinator, leaves, received);
                    pending_since_checkpoint = 0U;
                }
            });
        if (std::find(received.begin(), received.end(), false) !=
            received.end()) {
            throw std::runtime_error(
                "durable run ended before every block completed");
        }
        store.checkpoint(coordinator, leaves, received,
                         DurableRunStatus::Complete);
    }

    const AggregateStats aggregate = reduce_block_results(leaves);
    return DurableRunResult{
        RunResult{aggregate,
                  spec.total_scenarios,
                  static_cast<std::uint64_t>(recovery.blocks.size()),
                  workers_used},
        store.metadata().run_id,
        store.recovery_state().manifest_sequence,
        store.recovery_state().run_incarnation,
        recovered_blocks,
        computed_blocks,
        recovered_scenarios,
        computed_scenarios,
        store.recovery_state().resumed,
    };
}

}  // namespace mc
