#include "mc/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mc {

void RuntimeMetrics::reset(std::size_t block_count,
                           std::size_t worker_capacity,
                           std::size_t checkpoint_capacity) {
    std::vector<WorkerMetrics> next_workers;
    next_workers.reserve(worker_capacity);
    std::vector<std::uint64_t> next_block_compute(block_count, 0U);
    std::vector<std::uint64_t> next_result_persist(block_count, 0U);
    std::vector<std::uint64_t> next_checkpoints;
    next_checkpoints.reserve(checkpoint_capacity);

    total_elapsed_ns = 0U;
    durable_open_ns = 0U;
    scheduler_assignment_wait_ns = 0U;
    coordinator_completion_wait_ns = 0U;
    coordinator_consume_ns = 0U;
    fixed_tree_reduce_ns = 0U;
    max_assignment_queue_depth = 0U;
    max_completion_queue_depth = 0U;
    workers.swap(next_workers);
    block_compute_ns.swap(next_block_compute);
    result_persist_ns.swap(next_result_persist);
    checkpoint_ns.swap(next_checkpoints);
    checkpoint_samples_dropped = 0U;
    durable_io = {};
}

std::optional<std::uint64_t> latency_percentile_ns(
    const std::vector<std::uint64_t>& samples,
    double quantile) {
    if (!std::isfinite(quantile) || quantile < 0.0 || quantile > 1.0) {
        throw std::invalid_argument("latency quantile must be in [0, 1]");
    }
    std::vector<std::uint64_t> observed;
    observed.reserve(samples.size());
    for (const std::uint64_t sample : samples) {
        if (sample != 0U) {
            observed.push_back(sample);
        }
    }
    if (observed.empty()) {
        return std::nullopt;
    }
    const double rank = std::ceil(
        quantile * static_cast<double>(observed.size()));
    const std::size_t index =
        rank <= 1.0 ? 0U : static_cast<std::size_t>(rank - 1.0);
    std::nth_element(observed.begin(),
                     observed.begin() + static_cast<std::ptrdiff_t>(index),
                     observed.end());
    return observed[index];
}

}  // namespace mc
