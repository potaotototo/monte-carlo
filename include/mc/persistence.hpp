#pragma once

#include "mc/coordinator.hpp"
#include "mc/failure_injection.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mc {

class FailureInjector;

inline constexpr std::uint32_t kStorageSchemaVersion = 1;
// Zero selects the automatic full-snapshot cadence: at least 64 blocks per
// checkpoint and at most 1,024 periodic manifests for the whole run.
inline constexpr std::uint64_t kDefaultCheckpointIntervalBlocks = 0;
inline constexpr std::uint64_t kMinimumAutomaticCheckpointBlocks = 64;
inline constexpr std::uint64_t kMaximumAutomaticPeriodicManifests = 1'024;
inline constexpr std::uint64_t kDefaultMaxStorageBytes =
    std::uint64_t{64} * 1024U * 1024U * 1024U;
inline constexpr std::uint64_t kDefaultMaxStorageFiles = 2'000'100;
inline constexpr std::uint64_t kDefaultMinFreeSpaceBytes =
    std::uint64_t{64} * 1024U * 1024U;
inline constexpr std::uint64_t kDefaultMaxManifestBytes =
    std::uint64_t{128} * 1024U * 1024U;

enum class DurableRunStatus : std::uint8_t {
    Running = 1,
    Complete = 2,
    Failed = 3,
};

struct RunStoreConfig {
    std::filesystem::path run_directory;
    std::uint64_t checkpoint_interval_blocks =
        kDefaultCheckpointIntervalBlocks;
    std::uint64_t max_storage_bytes = kDefaultMaxStorageBytes;
    std::uint64_t max_storage_files = kDefaultMaxStorageFiles;
    std::uint64_t min_free_space_bytes = kDefaultMinFreeSpaceBytes;
    std::uint64_t max_manifest_bytes = kDefaultMaxManifestBytes;
    std::optional<FailureInjectionConfig> failure_injection;

    void validate() const;
};

struct RunMetadata {
    Sha256Digest run_id{};
    RunSpec spec;
    std::uint64_t block_size = 0;
    std::uint64_t block_count = 0;
    Sha256Digest run_spec_hash{};
    Sha256Digest execution_layout_hash{};
    Sha256Digest build_fingerprint{};
    std::string build_description;
};

struct DurableBlockRecord {
    Sha256Digest run_id{};
    BlockResult result;
};

struct ManifestEntry {
    std::uint64_t block_id = 0;
    std::uint64_t result_incarnation = 0;
    std::uint64_t lease_epoch = 0;
    Sha256Digest payload_checksum{};
};

struct FailureRecord {
    ValidationStatus status = ValidationStatus::DeterminismError;
    std::uint64_t block_id = 0;
    std::uint64_t run_incarnation = 0;
    std::uint64_t lease_epoch = 0;
    Sha256Digest observed_checksum{};
    Sha256Digest committed_checksum{};
    std::string reason;
};

struct RunManifest {
    Sha256Digest run_id{};
    Sha256Digest run_spec_hash{};
    Sha256Digest execution_layout_hash{};
    Sha256Digest build_fingerprint{};
    std::uint64_t sequence = 0;
    DurableRunStatus status = DurableRunStatus::Running;
    std::uint64_t run_incarnation = 1;
    std::uint64_t rng_version = kRngVersion;
    std::uint32_t stats_schema_version = kStatsSchemaVersion;
    std::uint64_t block_count = 0;
    std::vector<std::uint64_t> lease_epochs;
    std::vector<ManifestEntry> committed_blocks;
    AggregateStats committed_aggregate;
    std::optional<FailureRecord> failure;
};

// CRC32C is a fast accidental-corruption check for durable envelopes. SHA-256
// remains the logical content identity for stochastic and aggregate payloads.
std::uint32_t crc32c(const std::vector<std::uint8_t>& bytes);

Sha256Digest durable_run_id(const Sha256Digest& run_spec_hash,
                            const Sha256Digest& execution_layout_hash);

std::vector<std::uint8_t> encode_run_metadata(const RunMetadata& metadata);
RunMetadata decode_run_metadata(const std::vector<std::uint8_t>& bytes);

std::vector<std::uint8_t> encode_block_record(
    const DurableBlockRecord& record);
DurableBlockRecord decode_block_record(const std::vector<std::uint8_t>& bytes);

std::vector<std::uint8_t> encode_manifest(const RunManifest& manifest);
RunManifest decode_manifest(const std::vector<std::uint8_t>& bytes,
                            std::uint64_t max_manifest_bytes =
                                kDefaultMaxManifestBytes);

struct DurableRecoveryState {
    std::uint64_t manifest_sequence = 0;
    std::uint64_t run_incarnation = 1;
    DurableRunStatus status = DurableRunStatus::Running;
    bool resumed = false;
    std::vector<ScenarioBlock> blocks;
    std::vector<std::optional<BlockResult>> committed_results;
};

class DurableRunStore {
public:
    static DurableRunStore open(const RunSpec& spec,
                                const EngineConfig& engine_config,
                                const RunStoreConfig& store_config);

    DurableRunStore(const DurableRunStore&) = delete;
    DurableRunStore& operator=(const DurableRunStore&) = delete;
    ~DurableRunStore();
    DurableRunStore(DurableRunStore&& other) noexcept;
    DurableRunStore& operator=(DurableRunStore&& other) noexcept;

    [[nodiscard]] const RunMetadata& metadata() const noexcept;
    [[nodiscard]] const DurableRecoveryState& recovery_state() const noexcept;
    [[nodiscard]] const RunStoreConfig& config() const noexcept;

    // Makes an immutable block-result file durable. It is not committed until
    // a subsequent checkpoint manifest names it.
    void record_result(const BlockResult& result);

    // Installs a full-snapshot manifest. Every received leaf must have a
    // durable result record, and every durable result named by the manifest is
    // validated again before serialization.
    void checkpoint(const CoordinatorState& coordinator,
                    const std::vector<AggregateStats>& leaves,
                    const std::vector<bool>& received,
                    DurableRunStatus status = DurableRunStatus::Running,
                    std::optional<FailureRecord> failure = std::nullopt);

    [[nodiscard]] std::filesystem::path manifest_path(
        std::uint64_t sequence) const;
    [[nodiscard]] std::filesystem::path block_result_path(
        const ScenarioBlock& block) const;

private:
    DurableRunStore(RunSpec spec,
                    EngineConfig engine_config,
                    RunStoreConfig store_config,
                    RunMetadata metadata,
                    DurableRecoveryState recovery_state,
                    std::uint64_t current_storage_bytes,
                    std::uint64_t current_storage_files,
                    int lock_descriptor,
                    std::unique_ptr<FailureInjector> failure_injector);

    RunSpec spec_;
    EngineConfig engine_config_;
    RunStoreConfig store_config_;
    RunMetadata metadata_;
    DurableRecoveryState recovery_state_;
    std::vector<std::optional<BlockResult>> durable_results_;
    std::uint64_t current_storage_bytes_ = 0;
    std::uint64_t current_storage_files_ = 0;
    int lock_descriptor_ = -1;
    std::unique_ptr<FailureInjector> failure_injector_;
};

struct DurableRunResult {
    RunResult run_result;
    Sha256Digest run_id{};
    std::uint64_t manifest_sequence = 0;
    std::uint64_t run_incarnation = 0;
    std::uint64_t recovered_blocks = 0;
    std::uint64_t computed_blocks = 0;
    std::uint64_t recovered_scenarios = 0;
    std::uint64_t computed_scenarios = 0;
    bool resumed = false;
};

DurableRunResult run_parallel_durable(const RunSpec& spec,
                                      const EngineConfig& engine_config,
                                      const RunStoreConfig& store_config);

}  // namespace mc
