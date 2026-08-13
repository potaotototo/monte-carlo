#pragma once

#include "mc/engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string_view>

namespace mc {

inline constexpr int kFailureInjectionExitCode = 86;
inline constexpr std::uint32_t kReplayDescriptorVersion = 2;
inline constexpr std::size_t kMaxReplayDescriptorBytes = 64U * 1024U;
inline constexpr std::size_t kMaxReplayDescriptorLineBytes = 4U * 1024U;
inline constexpr std::uint64_t kNoFailureContext =
    std::numeric_limits<std::uint64_t>::max();

enum class FailurePoint : std::uint8_t {
    ResultBeforeFileFsync = 1,
    ResultAfterFileFsync = 2,
    ResultBeforeRename = 3,
    ResultAfterRename = 4,
    ManifestBeforeFileFsync = 5,
    ManifestAfterFileFsync = 6,
    ManifestBeforeRename = 7,
    ManifestAfterRename = 8,
    ManifestAfterInstallBeforeMemory = 9,
};

inline constexpr std::array<FailurePoint, 9> kFailurePoints = {
    FailurePoint::ResultBeforeFileFsync,
    FailurePoint::ResultAfterFileFsync,
    FailurePoint::ResultBeforeRename,
    FailurePoint::ResultAfterRename,
    FailurePoint::ManifestBeforeFileFsync,
    FailurePoint::ManifestAfterFileFsync,
    FailurePoint::ManifestBeforeRename,
    FailurePoint::ManifestAfterRename,
    FailurePoint::ManifestAfterInstallBeforeMemory,
};

[[nodiscard]] std::string_view failure_point_name(FailurePoint point) noexcept;
FailurePoint parse_failure_point(std::string_view name);

struct FailureInjectionConfig {
    FailurePoint selected_point = FailurePoint::ResultBeforeFileFsync;
    std::uint64_t selected_occurrence = 1;
    std::uint64_t failure_seed = 0;
    // Reserved for a future multi-worker deterministic scheduler. R3's
    // replayable crash harness uses one worker and records zero here.
    std::uint64_t deterministic_scheduler_seed = 0;
    std::filesystem::path replay_descriptor_path;

    void validate() const;
};

struct ReplayDescriptor {
    std::uint32_t version = kReplayDescriptorVersion;
    RunSpec spec;
    EngineConfig engine_config;
    std::uint64_t checkpoint_interval_blocks = 0;
    FailureInjectionConfig injection;
    Sha256Digest run_spec_hash{};
    Sha256Digest build_fingerprint{};
    std::uint64_t max_storage_bytes = 0;
    std::uint64_t max_storage_files = 0;
    std::uint64_t min_free_space_bytes = 0;
    std::uint64_t max_manifest_bytes = 0;
    Sha256Digest observed_trace_hash{};
    std::uint64_t observed_trace_events = 0;
    std::uint64_t run_incarnation = kNoFailureContext;
    std::uint64_t block_id = kNoFailureContext;
    std::uint64_t checkpoint_sequence = kNoFailureContext;
};

FailureInjectionConfig failure_injection_from_seed(
    std::uint64_t failure_seed,
    std::filesystem::path replay_descriptor_path);

ReplayDescriptor read_replay_descriptor(
    const std::filesystem::path& path);

}  // namespace mc
