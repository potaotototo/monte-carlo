#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mc {

struct WorkerMetrics {
    std::uint64_t blocks_completed = 0;
    std::uint64_t scenarios_completed = 0;
    std::uint64_t assignment_wait_ns = 0;
    std::uint64_t compute_ns = 0;
    std::uint64_t completion_queue_wait_ns = 0;
};

struct DurableIoMetrics {
    std::uint64_t metadata_files_installed = 0;
    std::uint64_t result_files_installed = 0;
    std::uint64_t manifest_files_installed = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t write_ns = 0;
    std::uint64_t file_fsync_ns = 0;
    std::uint64_t rename_ns = 0;
    std::uint64_t directory_fsync_ns = 0;
};

struct RuntimeMetrics {
    std::uint64_t total_elapsed_ns = 0;
    std::uint64_t durable_open_ns = 0;
    std::uint64_t scheduler_assignment_wait_ns = 0;
    std::uint64_t coordinator_completion_wait_ns = 0;
    std::uint64_t coordinator_consume_ns = 0;
    std::uint64_t fixed_tree_reduce_ns = 0;
    std::size_t max_assignment_queue_depth = 0;
    std::size_t max_completion_queue_depth = 0;
    std::vector<WorkerMetrics> workers;
    // Zero means the block was recovered or not executed in this invocation.
    std::vector<std::uint64_t> block_compute_ns;
    // Zero means no new durable result was installed for this block.
    std::vector<std::uint64_t> result_persist_ns;
    std::vector<std::uint64_t> checkpoint_ns;
    DurableIoMetrics durable_io;

    void reset(std::size_t block_count);
};

// Uses the nearest-rank definition and ignores zero sentinel entries.
std::optional<std::uint64_t> latency_percentile_ns(
    const std::vector<std::uint64_t>& samples,
    double quantile);

}  // namespace mc
