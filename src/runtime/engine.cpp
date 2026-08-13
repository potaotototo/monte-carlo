#include "mc/engine.hpp"

#include "mc/bounded_queue.hpp"
#include "mc/codec.hpp"
#include "mc/coordinator.hpp"
#include "mc/model.hpp"
#include "mc/persistence.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace mc {
namespace {

using MetricsClock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(MetricsClock::time_point started) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        MetricsClock::now() - started).count();
    // Zero is reserved for a missing sample. A clock whose resolution is
    // coarser than the measured operation records the minimum observable unit.
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 1U;
}

std::uint64_t clock_marker_ns() noexcept {
    const auto marker = std::chrono::duration_cast<std::chrono::nanoseconds>(
        MetricsClock::now().time_since_epoch()).count();
    return marker > 0 ? static_cast<std::uint64_t>(marker) : 1U;
}

std::uint64_t elapsed_since_marker_ns(std::uint64_t started) noexcept {
    const std::uint64_t stopped = clock_marker_ns();
    return stopped > started ? stopped - started : 1U;
}

void add_metric(std::uint64_t& total, std::uint64_t value) noexcept {
    total = value > std::numeric_limits<std::uint64_t>::max() - total
                ? std::numeric_limits<std::uint64_t>::max()
                : total + value;
}

void clear_commit_samples(RuntimeMetrics* metrics) noexcept {
    if (metrics != nullptr) {
        std::fill(metrics->block_commit_ns.begin(),
                  metrics->block_commit_ns.end(), 0U);
    }
}

class TotalMetricsTimer {
public:
    TotalMetricsTimer(RuntimeMetrics* metrics,
                      MetricsClock::time_point started) noexcept
        : metrics_(metrics), started_(started) {}

    ~TotalMetricsTimer() {
        if (metrics_ != nullptr) {
            metrics_->total_elapsed_ns = elapsed_ns(started_);
        }
    }

private:
    RuntimeMetrics* metrics_;
    MetricsClock::time_point started_;
};

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
    const std::function<void(BlockResult&&)>& consume_result,
    RuntimeMetrics* metrics) {
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

    BoundedQueue<ScenarioBlock> assignments(
        assignment_capacity,
        metrics == nullptr ? nullptr : &metrics->max_assignment_queue_depth);
    BoundedQueue<BlockResult> completions(
        completion_capacity,
        metrics == nullptr ? nullptr : &metrics->max_completion_queue_depth);
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
    if (metrics != nullptr) {
        if (metrics->workers.capacity() < actual_worker_count) {
            throw std::logic_error(
                "runtime metrics worker capacity was not preallocated");
        }
        metrics->workers.assign(actual_worker_count, {});
        metrics->max_reduction_backlog_blocks =
            blocks.size() - missing_count;
        metrics->max_reduction_backlog_bytes =
            metrics->max_reduction_backlog_blocks * sizeof(AggregateStats);
    }
    try {
        for (std::size_t worker_index = 0;
             worker_index < actual_worker_count;
             ++worker_index) {
            workers.emplace_back([&, worker_index] {
                WorkerMetrics worker_metrics;
                try {
                    const GbmKernel kernel(spec);
                    ScenarioBlock block;
                    for (;;) {
                        std::uint64_t blocked_ns = 0U;
                        const bool assigned = assignments.pop(
                            block, metrics == nullptr ? nullptr : &blocked_ns);
                        if (metrics != nullptr) {
                            add_metric(worker_metrics.assignment_wait_ns,
                                       blocked_ns);
                        }
                        if (!assigned) {
                            break;
                        }
                        MetricsClock::time_point compute_started{};
                        if (metrics != nullptr) {
                            compute_started = MetricsClock::now();
                        }
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
                        if (metrics != nullptr) {
                            const std::uint64_t compute_ns =
                                elapsed_ns(compute_started);
                            add_metric(worker_metrics.compute_ns, compute_ns);
                            ++worker_metrics.blocks_completed;
                            worker_metrics.scenarios_completed +=
                                block.end_scenario - block.start_scenario;
                            metrics->block_compute_ns[
                                static_cast<std::size_t>(block.block_id)] =
                                compute_ns;
                        }
                        blocked_ns = 0U;
                        if (metrics != nullptr) {
                            metrics->block_commit_ns[
                                static_cast<std::size_t>(block.block_id)] =
                                clock_marker_ns();
                        }
                        const bool submitted = completions.push(
                            std::move(result),
                            metrics == nullptr ? nullptr : &blocked_ns);
                        if (metrics != nullptr) {
                            add_metric(worker_metrics.completion_queue_wait_ns,
                                       blocked_ns);
                        }
                        if (!submitted) {
                            break;
                        }
                    }
                } catch (...) {
                    record_error(std::current_exception());
                }

                if (metrics != nullptr) {
                    metrics->workers[worker_index] = worker_metrics;
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
        clear_commit_samples(metrics);
        throw;
    }

    std::thread scheduler;
    std::uint64_t scheduler_assignment_wait_ns = 0U;
    try {
        scheduler = std::thread([&] {
            try {
                for (const std::size_t index : pending_indices) {
                    std::uint64_t blocked_ns = 0U;
                    const bool submitted = assignments.push(
                        blocks[index],
                        metrics == nullptr ? nullptr : &blocked_ns);
                    if (metrics != nullptr) {
                        add_metric(scheduler_assignment_wait_ns, blocked_ns);
                    }
                    if (!submitted) {
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
    std::uint64_t coordinator_completion_wait_ns = 0U;
    std::uint64_t coordinator_consume_ns = 0U;
    for (;;) {
        std::uint64_t blocked_ns = 0U;
        const bool available = completions.pop(
            completed, metrics == nullptr ? nullptr : &blocked_ns);
        if (metrics != nullptr) {
            add_metric(coordinator_completion_wait_ns, blocked_ns);
        }
        if (!available) {
            break;
        }
        MetricsClock::time_point consume_started{};
        if (metrics != nullptr) {
            consume_started = MetricsClock::now();
        }
        try {
            const std::size_t completed_index = static_cast<std::size_t>(
                completed.block.block_id);
            consume_result(std::move(completed));
            if (metrics != nullptr) {
                metrics->block_commit_ns[completed_index] =
                    elapsed_since_marker_ns(
                        metrics->block_commit_ns[completed_index]);
                ++metrics->max_reduction_backlog_blocks;
                metrics->max_reduction_backlog_bytes =
                    metrics->max_reduction_backlog_blocks *
                    sizeof(AggregateStats);
            }
        } catch (...) {
            if (metrics != nullptr) {
                add_metric(coordinator_consume_ns, elapsed_ns(consume_started));
            }
            record_error(std::current_exception());
            break;
        }
        if (metrics != nullptr) {
            add_metric(coordinator_consume_ns, elapsed_ns(consume_started));
        }
    }

    scheduler.join();
    for (std::thread& worker : workers) {
        worker.join();
    }
    if (metrics != nullptr) {
        metrics->scheduler_assignment_wait_ns =
            scheduler_assignment_wait_ns;
        metrics->coordinator_completion_wait_ns =
            coordinator_completion_wait_ns;
        metrics->coordinator_consume_ns = coordinator_consume_ns;
    }
    if (first_error) {
        // Publication timestamps are staged in the output slots to avoid a
        // fourth per-block allocation. Never expose those markers as latency
        // samples when execution fails before all accepted blocks are timed.
        clear_commit_samples(metrics);
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
    if (block_count > config.max_materialized_blocks ||
        block_count > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error(
            "run requires " + std::to_string(block_count) +
            " blocks, exceeding the host/materialization limit of " +
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

RunResult run_parallel(const RunSpec& spec,
                       const EngineConfig& config,
                       RuntimeMetrics* metrics) {
    MetricsClock::time_point total_started{};
    if (metrics != nullptr) {
        total_started = MetricsClock::now();
        metrics->reset(0U);
    }
    const TotalMetricsTimer total_timer(metrics, total_started);
    const std::vector<ScenarioBlock> blocks = make_blocks(spec, config);
    if (metrics != nullptr) {
        metrics->reset(blocks.size(),
                       std::min(config.worker_count, blocks.size()));
    }
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
        },
        metrics);
    if (std::find(received.begin(), received.end(), false) != received.end()) {
        throw std::runtime_error("parallel run ended before every block completed");
    }

    MetricsClock::time_point reduce_started{};
    if (metrics != nullptr) {
        reduce_started = MetricsClock::now();
    }
    const AggregateStats aggregate = reduce_block_results(leaves);
    if (metrics != nullptr) {
        metrics->fixed_tree_reduce_ns = elapsed_ns(reduce_started);
    }
    return RunResult{
        aggregate,
        spec.total_scenarios,
        static_cast<std::uint64_t>(blocks.size()),
        actual_worker_count,
    };
}

DurableRunResult run_parallel_durable(
    const RunSpec& spec,
    const EngineConfig& engine_config,
    const RunStoreConfig& store_config,
    RuntimeMetrics* metrics) {
    MetricsClock::time_point total_started{};
    if (metrics != nullptr) {
        total_started = MetricsClock::now();
        metrics->reset(0U);
    }
    const TotalMetricsTimer total_timer(metrics, total_started);
    DurableRunStore store = DurableRunStore::open(
        spec, engine_config, store_config, metrics);
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
    if (metrics != nullptr) {
        metrics->max_reduction_backlog_blocks =
            static_cast<std::size_t>(recovered_blocks);
        metrics->max_reduction_backlog_bytes =
            metrics->max_reduction_backlog_blocks * sizeof(AggregateStats);
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
                                     DurableRunStatus::Failed,
                                     std::move(failure));
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
            },
            metrics);
        if (std::find(received.begin(), received.end(), false) !=
            received.end()) {
            throw std::runtime_error(
                "durable run ended before every block completed");
        }
        store.checkpoint(coordinator, leaves, received,
                         DurableRunStatus::Complete);
    }

    MetricsClock::time_point reduce_started{};
    if (metrics != nullptr) {
        reduce_started = MetricsClock::now();
    }
    const AggregateStats aggregate = reduce_block_results(leaves);
    if (metrics != nullptr) {
        metrics->fixed_tree_reduce_ns = elapsed_ns(reduce_started);
    }
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
