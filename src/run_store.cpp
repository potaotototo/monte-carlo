#include "mc/persistence.hpp"

#include "mc/codec.hpp"
#include "mc/identity.hpp"

#include "failure_injection_internal.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <sys/file.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mc {
namespace {

constexpr const char* kRunSpecFilename = "run_spec.bin";
constexpr const char* kManifestDirectory = "manifests";
constexpr const char* kBlockDirectory = "block_results";
constexpr const char* kTemporaryDirectory = "tmp";
constexpr const char* kLockFilename = ".run.lock";

class InvalidDurableArtifact : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class SizeLimitKind : std::uint8_t {
    Schema,
    OperatorPolicy,
};

enum class DurableArtifactKind : std::uint8_t {
    Metadata,
    BlockResult,
    Manifest,
};

struct DurableWriteContext {
    DurableArtifactKind kind = DurableArtifactKind::Metadata;
    FailureContext failure;
};

using MetricsClock = std::chrono::steady_clock;

std::uint64_t metrics_elapsed_ns(MetricsClock::time_point started) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        MetricsClock::now() - started).count();
    // Zero is reserved for a missing sample. A clock whose resolution is
    // coarser than the measured operation records the minimum observable unit.
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 1U;
}

void add_metric(std::uint64_t& total, std::uint64_t value) noexcept {
    total = value > std::numeric_limits<std::uint64_t>::max() - total
                ? std::numeric_limits<std::uint64_t>::max()
                : total + value;
}

void hit_write_failure(FailureInjector* injector,
                       const DurableWriteContext& context,
                       FailurePoint result_point,
                       FailurePoint manifest_point) {
    if (injector == nullptr ||
        context.kind == DurableArtifactKind::Metadata) {
        return;
    }
    injector->hit(context.kind == DurableArtifactKind::BlockResult
                      ? result_point
                      : manifest_point,
                  context.failure);
}

std::runtime_error system_error(const std::string& operation,
                                const std::filesystem::path& path,
                                int error_number = errno) {
    return std::runtime_error(operation + " " + path.string() + ": " +
                              std::strerror(error_number));
}

std::string padded_number(std::uint64_t value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setw(20) << std::setfill('0') << value;
    return stream.str();
}

class ScopedRunLock {
public:
    explicit ScopedRunLock(const std::filesystem::path& path) {
        descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (descriptor_ < 0) {
            throw system_error("cannot open run-store lock", path);
        }
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int saved_error = errno;
            static_cast<void>(::close(descriptor_));
            descriptor_ = -1;
            if (saved_error == EWOULDBLOCK || saved_error == EAGAIN) {
                throw std::runtime_error(
                    "run directory is already owned by another coordinator");
            }
            throw system_error("cannot lock run store", path, saved_error);
        }
    }

    ScopedRunLock(const ScopedRunLock&) = delete;
    ScopedRunLock& operator=(const ScopedRunLock&) = delete;

    ~ScopedRunLock() {
        if (descriptor_ >= 0) {
            static_cast<void>(::flock(descriptor_, LOCK_UN));
            static_cast<void>(::close(descriptor_));
        }
    }

    int release() noexcept {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }

private:
    int descriptor_ = -1;
};

std::filesystem::path manifest_path_for(const std::filesystem::path& root,
                                        std::uint64_t sequence) {
    return root / kManifestDirectory /
           ("manifest_" + padded_number(sequence) + ".bin");
}

std::filesystem::path block_path_for(const std::filesystem::path& root,
                                     const ScenarioBlock& block) {
    return root / kBlockDirectory /
           ("block_" + padded_number(block.block_id) + "_inc_" +
            padded_number(block.run_incarnation) + "_epoch_" +
            padded_number(block.lease_epoch) + ".bin");
}

std::optional<std::uint64_t> parse_manifest_sequence(
    const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    constexpr std::string_view prefix = "manifest_";
    constexpr std::string_view suffix = ".bin";
    constexpr std::size_t digits = 20U;
    if (name.size() != prefix.size() + digits + suffix.size() ||
        name.compare(0U, prefix.size(), prefix) != 0 ||
        name.compare(prefix.size() + digits, suffix.size(), suffix) != 0) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char* begin = name.data() + static_cast<std::ptrdiff_t>(prefix.size());
    const char* end = begin + static_cast<std::ptrdiff_t>(digits);
    const auto [position, error] = std::from_chars(begin, end, value, 10);
    if (error != std::errc{} || position != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<ScenarioBlock> parse_block_result_filename(
    const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    constexpr std::string_view block_prefix = "block_";
    constexpr std::string_view incarnation_separator = "_inc_";
    constexpr std::string_view epoch_separator = "_epoch_";
    constexpr std::string_view suffix = ".bin";
    constexpr std::size_t digits = 20U;
    const std::size_t incarnation_offset = block_prefix.size() + digits;
    const std::size_t incarnation_digits =
        incarnation_offset + incarnation_separator.size();
    const std::size_t epoch_offset = incarnation_digits + digits;
    const std::size_t epoch_digits = epoch_offset + epoch_separator.size();
    const std::size_t suffix_offset = epoch_digits + digits;
    if (name.size() != suffix_offset + suffix.size() ||
        name.compare(0U, block_prefix.size(), block_prefix) != 0 ||
        name.compare(incarnation_offset, incarnation_separator.size(),
                     incarnation_separator) != 0 ||
        name.compare(epoch_offset, epoch_separator.size(), epoch_separator) != 0 ||
        name.compare(suffix_offset, suffix.size(), suffix) != 0) {
        return std::nullopt;
    }
    const auto parse_field = [&](std::size_t offset)
        -> std::optional<std::uint64_t> {
        std::uint64_t value = 0;
        const char* begin = name.data() + static_cast<std::ptrdiff_t>(offset);
        const char* end = begin + static_cast<std::ptrdiff_t>(digits);
        const auto [position, error] = std::from_chars(begin, end, value, 10);
        if (error != std::errc{} || position != end) {
            return std::nullopt;
        }
        return value;
    };
    const std::optional<std::uint64_t> block_id =
        parse_field(block_prefix.size());
    const std::optional<std::uint64_t> incarnation =
        parse_field(incarnation_digits);
    const std::optional<std::uint64_t> epoch = parse_field(epoch_digits);
    if (!block_id.has_value() || !incarnation.has_value() ||
        !epoch.has_value()) {
        return std::nullopt;
    }
    return ScenarioBlock{*block_id, 0U, 0U, *incarnation, *epoch};
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path,
                                    std::uint64_t maximum_size,
                                    SizeLimitKind size_limit_kind =
                                        SizeLimitKind::Schema) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            throw InvalidDurableArtifact(
                "durable file is missing: " + path.string());
        }
        throw std::runtime_error("cannot stat durable file " + path.string() +
                                 ": " + error.message());
    }
    if (size > maximum_size ||
        size > static_cast<std::uintmax_t>(
                   std::numeric_limits<std::size_t>::max())) {
        if (size_limit_kind == SizeLimitKind::OperatorPolicy) {
            throw std::length_error(
                "durable file exceeds the operator-supplied size limit: " +
                path.string());
        }
        throw InvalidDurableArtifact(
            "durable file exceeds its storage-schema size limit: " +
            path.string());
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open durable file for reading: " +
                                 path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream || stream.peek() != std::ifstream::traits_type::eof()) {
        throw InvalidDurableArtifact("short or changing durable file read: " +
                                     path.string());
    }
    return bytes;
}

bool has_existing_run_payload(const std::filesystem::path& root) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(
             root, std::filesystem::directory_options::none, error),
         end;
         iterator != end;
         iterator.increment(error)) {
        if (error) {
            throw std::runtime_error("cannot inspect prospective run directory: " +
                                     error.message());
        }
        if (!iterator->is_regular_file(error)) {
            if (error) {
                throw std::runtime_error(
                    "cannot inspect prospective run-store entry: " +
                    error.message());
            }
            continue;
        }
        if (iterator->path().parent_path() == root &&
            iterator->path().filename() == kLockFilename) {
            continue;
        }
        return true;
    }
    return false;
}

void sync_directory(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        throw system_error("cannot open directory for fsync", path);
    }
    if (::fsync(descriptor) != 0) {
        const int saved_error = errno;
        static_cast<void>(::close(descriptor));
        throw system_error("cannot fsync directory", path, saved_error);
    }
    if (::close(descriptor) != 0) {
        throw system_error("cannot close directory", path);
    }
}

void require_same_filesystem(const std::filesystem::path& first,
                             const std::filesystem::path& second) {
    struct stat first_stat {};
    struct stat second_stat {};
    if (::stat(first.c_str(), &first_stat) != 0) {
        throw system_error("cannot stat directory", first);
    }
    if (::stat(second.c_str(), &second_stat) != 0) {
        throw system_error("cannot stat directory", second);
    }
    if (first_stat.st_dev != second_stat.st_dev) {
        throw std::runtime_error(
            "durable tmp and destination directories are on different filesystems");
    }
}

std::pair<std::uint64_t, std::uint64_t> storage_usage(
    const std::filesystem::path& root) {
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::none, error);
    if (error) {
        throw std::runtime_error("cannot scan run directory: " + error.message());
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            throw std::runtime_error("cannot scan run directory: " + error.message());
        }
        if (!iterator->is_regular_file(error)) {
            if (error) {
                throw std::runtime_error("cannot inspect run-store entry: " +
                                         error.message());
            }
            continue;
        }
        const std::uintmax_t file_bytes = iterator->file_size(error);
        if (error || file_bytes >
                         static_cast<std::uintmax_t>(
                             std::numeric_limits<std::uint64_t>::max()) ||
            bytes > std::numeric_limits<std::uint64_t>::max() - file_bytes) {
            throw std::runtime_error("run-store size cannot be represented safely");
        }
        bytes += static_cast<std::uint64_t>(file_bytes);
        if (files == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("run-store file count overflow");
        }
        ++files;
    }
    return {bytes, files};
}

void remove_stale_temporary_files(const std::filesystem::path& directory) {
    bool removed = false;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         iterator != end;
         iterator.increment(error)) {
        if (error) {
            throw std::runtime_error("cannot scan durable tmp directory: " +
                                     error.message());
        }
        const std::string name = iterator->path().filename().string();
        if (name.find(".bin.tmp.") == std::string::npos &&
            name.find("run_spec.bin.tmp.") == std::string::npos) {
            continue;
        }
        if (!iterator->is_regular_file(error)) {
            if (error) {
                throw std::runtime_error(
                    "cannot inspect stale durable temp file: " +
                    error.message());
            }
            continue;
        }
        error.clear();
        if (!std::filesystem::remove(iterator->path(), error) || error) {
            throw std::runtime_error("cannot remove stale durable temp file: " +
                                     error.message());
        }
        removed = true;
    }
    if (removed) {
        sync_directory(directory);
    }
}

void remove_orphan_block_results(
    const std::filesystem::path& directory,
    const std::vector<std::optional<BlockResult>>& committed_results) {
    bool removed = false;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         iterator != end;
         iterator.increment(error)) {
        if (error) {
            throw std::runtime_error("cannot scan block-result directory: " +
                                     error.message());
        }
        if (!iterator->is_regular_file(error)) {
            if (error) {
                throw std::runtime_error("cannot inspect block-result file: " +
                                         error.message());
            }
            continue;
        }
        const std::optional<ScenarioBlock> named =
            parse_block_result_filename(iterator->path());
        if (!named.has_value()) {
            continue;
        }
        bool referenced = false;
        if (named->block_id < committed_results.size()) {
            const std::optional<BlockResult>& result =
                committed_results[static_cast<std::size_t>(named->block_id)];
            referenced = result.has_value() &&
                         result->block.run_incarnation ==
                             named->run_incarnation &&
                         result->block.lease_epoch == named->lease_epoch;
        }
        if (referenced) {
            continue;
        }
        error.clear();
        if (!std::filesystem::remove(iterator->path(), error) || error) {
            throw std::runtime_error("cannot remove orphan block result: " +
                                     error.message());
        }
        removed = true;
    }
    if (removed) {
        sync_directory(directory);
    }
}

std::uint64_t checked_add(std::uint64_t left,
                          std::uint64_t right,
                          const char* label) {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        throw std::length_error(std::string(label) + " estimate overflow");
    }
    return left + right;
}

std::uint64_t checked_multiply(std::uint64_t left,
                               std::uint64_t right,
                               const char* label) {
    if (left != 0U &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        throw std::length_error(std::string(label) + " estimate overflow");
    }
    return left * right;
}

RunStoreConfig resolve_checkpoint_cadence(const RunStoreConfig& requested,
                                          std::uint64_t block_count) {
    RunStoreConfig resolved = requested;
    if (resolved.checkpoint_interval_blocks != 0U) {
        return resolved;
    }
    const std::uint64_t cadence_for_manifest_cap =
        1U + (block_count - 1U) /
                 kMaximumAutomaticPeriodicManifests;
    resolved.checkpoint_interval_blocks = std::max(
        kMinimumAutomaticCheckpointBlocks, cadence_for_manifest_cap);
    return resolved;
}

void preflight_run_completion(const RunStoreConfig& config,
                              std::uint64_t current_bytes,
                              std::uint64_t current_files,
                              std::uint64_t block_count,
                              std::uint64_t missing_blocks,
                              std::uint64_t additional_manifests,
                              std::uint64_t additional_fixed_bytes = 0U,
                              std::uint64_t additional_fixed_files = 0U) {
    // A full manifest has an eight-byte lease for every block and at most one
    // 56-byte commit entry per block, plus a bounded fixed envelope/header.
    const std::uint64_t maximum_manifest = checked_add(
        512U, checked_multiply(block_count, 64U, "manifest size"),
        "manifest size");
    if (maximum_manifest > config.max_manifest_bytes) {
        throw std::length_error(
            "the complete full-snapshot manifest would exceed max_manifest_bytes");
    }
    std::uint64_t additional_bytes = checked_multiply(
        missing_blocks, 512U, "block-result storage");
    additional_bytes = checked_add(
        additional_bytes,
        checked_multiply(additional_manifests, maximum_manifest,
                         "manifest storage"),
        "durable completion storage");
    additional_bytes = checked_add(additional_bytes, additional_fixed_bytes,
                                   "durable completion storage");
    const std::uint64_t additional_files = checked_add(
        checked_add(missing_blocks, additional_manifests,
                    "durable completion file count"),
        additional_fixed_files, "durable completion file count");
    if (current_bytes > config.max_storage_bytes ||
        additional_bytes > config.max_storage_bytes - current_bytes) {
        throw std::length_error(
            "estimated durable completion size exceeds max_storage_bytes");
    }
    if (current_files > config.max_storage_files ||
        additional_files > config.max_storage_files - current_files) {
        throw std::length_error(
            "estimated durable completion file count exceeds max_storage_files");
    }
    const std::filesystem::space_info space =
        std::filesystem::space(config.run_directory);
    const std::uint64_t required = checked_add(
        additional_bytes, config.min_free_space_bytes,
        "durable completion free-space");
    if (space.available < required) {
        throw std::length_error(
            "estimated durable completion size exceeds available free space policy");
    }
}

void preflight_write(const RunStoreConfig& config,
                     std::uint64_t current_bytes,
                     std::uint64_t current_files,
                     std::size_t next_bytes) {
    if (next_bytes > std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("durable write size exceeds host limits");
    }
    const std::uint64_t next = static_cast<std::uint64_t>(next_bytes);
    if (next > config.max_storage_bytes ||
        current_bytes > config.max_storage_bytes - next) {
        throw std::runtime_error("durable write would exceed max_storage_bytes");
    }
    if (current_files >= config.max_storage_files) {
        throw std::runtime_error("durable write would exceed max_storage_files");
    }
    const std::filesystem::space_info space =
        std::filesystem::space(config.run_directory);
    const std::uint64_t required = next + config.min_free_space_bytes;
    if (required < config.min_free_space_bytes || space.available < required) {
        throw std::runtime_error(
            "durable write would violate min_free_space_bytes");
    }
}

void write_all(int descriptor,
               const std::vector<std::uint8_t>& bytes,
               const std::filesystem::path& path) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const std::size_t chunk = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t written =
            ::write(descriptor, bytes.data() + offset, chunk);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw system_error("cannot write durable temporary file", path);
        }
        if (written == 0) {
            throw std::runtime_error("zero-length durable write: " + path.string());
        }
        offset += static_cast<std::size_t>(written);
    }
}

void atomic_write(const RunStoreConfig& config,
                  const std::filesystem::path& final_path,
                  const std::vector<std::uint8_t>& bytes,
                  std::uint64_t& current_bytes,
                  std::uint64_t& current_files,
                  FailureInjector* failure_injector,
                  RuntimeMetrics* metrics,
                  const DurableWriteContext& write_context) {
    if (std::filesystem::exists(final_path)) {
        const std::vector<std::uint8_t> existing =
            read_file(final_path, static_cast<std::uint64_t>(bytes.size()) + 1U);
        if (existing == bytes) {
            return;
        }
        throw std::runtime_error("immutable durable file already exists with different bytes: " +
                                 final_path.string());
    }
    preflight_write(config, current_bytes, current_files, bytes.size());

    static std::atomic<std::uint64_t> nonce{0};
    const std::string temporary_name =
        final_path.filename().string() + ".tmp." +
        std::to_string(static_cast<std::uint64_t>(::getpid())) + "." +
        std::to_string(nonce.fetch_add(1U));
    const std::filesystem::path temporary_path =
        config.run_directory / kTemporaryDirectory / temporary_name;
    int descriptor = ::open(temporary_path.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0) {
        throw system_error("cannot create durable temporary file", temporary_path);
    }

    try {
        MetricsClock::time_point write_started{};
        if (metrics != nullptr) {
            write_started = MetricsClock::now();
        }
        write_all(descriptor, bytes, temporary_path);
        if (metrics != nullptr) {
            add_metric(metrics->durable_io.write_ns,
                       metrics_elapsed_ns(write_started));
        }
        hit_write_failure(failure_injector, write_context,
                          FailurePoint::ResultBeforeFileFsync,
                          FailurePoint::ManifestBeforeFileFsync);
        MetricsClock::time_point file_fsync_started{};
        if (metrics != nullptr) {
            file_fsync_started = MetricsClock::now();
        }
        if (::fsync(descriptor) != 0) {
            throw system_error("cannot fsync durable temporary file", temporary_path);
        }
        if (metrics != nullptr) {
            add_metric(metrics->durable_io.file_fsync_ns,
                       metrics_elapsed_ns(file_fsync_started));
        }
        hit_write_failure(failure_injector, write_context,
                          FailurePoint::ResultAfterFileFsync,
                          FailurePoint::ManifestAfterFileFsync);
        const int close_result = ::close(descriptor);
        descriptor = -1;
        if (close_result != 0) {
            throw system_error("cannot close durable temporary file", temporary_path);
        }
        hit_write_failure(failure_injector, write_context,
                          FailurePoint::ResultBeforeRename,
                          FailurePoint::ManifestBeforeRename);
        MetricsClock::time_point rename_started{};
        if (metrics != nullptr) {
            rename_started = MetricsClock::now();
        }
        if (::rename(temporary_path.c_str(), final_path.c_str()) != 0) {
            throw system_error("cannot atomically install durable file", final_path);
        }
        if (metrics != nullptr) {
            add_metric(metrics->durable_io.rename_ns,
                       metrics_elapsed_ns(rename_started));
        }
        hit_write_failure(failure_injector, write_context,
                          FailurePoint::ResultAfterRename,
                          FailurePoint::ManifestAfterRename);
        MetricsClock::time_point directory_fsync_started{};
        if (metrics != nullptr) {
            directory_fsync_started = MetricsClock::now();
        }
        sync_directory(final_path.parent_path());
        if (metrics != nullptr) {
            add_metric(metrics->durable_io.directory_fsync_ns,
                       metrics_elapsed_ns(directory_fsync_started));
        }
        if (final_path.parent_path() != temporary_path.parent_path()) {
            if (metrics != nullptr) {
                directory_fsync_started = MetricsClock::now();
            }
            sync_directory(temporary_path.parent_path());
            if (metrics != nullptr) {
                add_metric(metrics->durable_io.directory_fsync_ns,
                           metrics_elapsed_ns(directory_fsync_started));
            }
        }
    } catch (...) {
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
        }
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove(temporary_path, ignored));
        throw;
    }

    current_bytes += static_cast<std::uint64_t>(bytes.size());
    ++current_files;
    if (metrics != nullptr) {
        add_metric(metrics->durable_io.bytes_written,
                   static_cast<std::uint64_t>(bytes.size()));
        switch (write_context.kind) {
        case DurableArtifactKind::Metadata:
            ++metrics->durable_io.metadata_files_installed;
            break;
        case DurableArtifactKind::BlockResult:
            ++metrics->durable_io.result_files_installed;
            break;
        case DurableArtifactKind::Manifest:
            ++metrics->durable_io.manifest_files_installed;
            break;
        }
    }
}

bool aggregates_equal(const AggregateStats& left, const AggregateStats& right) {
    return left.n == right.n &&
           std::bit_cast<std::uint64_t>(left.mean) ==
               std::bit_cast<std::uint64_t>(right.mean) &&
           std::bit_cast<std::uint64_t>(left.m2) ==
               std::bit_cast<std::uint64_t>(right.m2) &&
           std::bit_cast<std::uint64_t>(left.min) ==
               std::bit_cast<std::uint64_t>(right.min) &&
           std::bit_cast<std::uint64_t>(left.max) ==
               std::bit_cast<std::uint64_t>(right.max);
}

RunMetadata expected_metadata(const RunSpec& spec,
                              const EngineConfig& config) {
    spec.validate();
    config.validate(spec);
    const std::uint64_t block_count =
        1U + (spec.total_scenarios - 1U) / config.block_size;
    if (block_count > config.max_materialized_blocks) {
        throw std::length_error("durable run exceeds max_materialized_blocks");
    }
    const BuildIdentity build = current_build_identity();
    RunMetadata metadata;
    metadata.spec = spec;
    metadata.block_size = config.block_size;
    metadata.block_count = block_count;
    metadata.run_spec_hash = run_spec_hash(spec);
    metadata.execution_layout_hash = execution_layout_hash(spec, config);
    metadata.build_fingerprint = build.hash;
    metadata.build_description = build.description;
    metadata.run_id = durable_run_id(metadata.run_spec_hash,
                                     metadata.execution_layout_hash);
    return metadata;
}

void require_compatible(const RunMetadata& persisted,
                        const RunMetadata& expected) {
    if (persisted.run_spec_hash != expected.run_spec_hash) {
        throw std::runtime_error(
            "run directory contains an incompatible run specification");
    }
    if (persisted.execution_layout_hash != expected.execution_layout_hash ||
        persisted.block_size != expected.block_size ||
        persisted.block_count != expected.block_count) {
        throw std::runtime_error(
            "run directory contains an incompatible execution layout");
    }
    if (persisted.build_fingerprint != expected.build_fingerprint ||
        persisted.build_description != expected.build_description) {
        throw std::runtime_error(
            "run directory was created by an incompatible build/runtime identity");
    }
    if (persisted.run_id != expected.run_id) {
        throw std::runtime_error("run directory contains an incompatible run ID");
    }
}

struct LoadedManifest {
    RunManifest manifest;
    std::vector<std::optional<BlockResult>> results;
};

LoadedManifest validate_manifest_snapshot(
    const RunManifest& manifest,
    const RunMetadata& metadata,
    const EngineConfig& engine_config,
    const RunStoreConfig& store_config) {
    if (manifest.run_id != metadata.run_id ||
        manifest.run_spec_hash != metadata.run_spec_hash ||
        manifest.execution_layout_hash != metadata.execution_layout_hash ||
        manifest.build_fingerprint != metadata.build_fingerprint ||
        manifest.rng_version != metadata.spec.rng_version ||
        manifest.stats_schema_version != metadata.spec.stats_schema_version ||
        manifest.block_count != metadata.block_count) {
        throw InvalidDurableArtifact(
            "manifest is incompatible with run metadata");
    }

    std::vector<ScenarioBlock> canonical =
        make_blocks(metadata.spec, engine_config);
    std::vector<std::optional<BlockResult>> results(canonical.size());
    std::vector<AggregateStats> leaves(canonical.size());
    std::vector<bool> received(canonical.size(), false);
    for (const ManifestEntry& entry : manifest.committed_blocks) {
        const std::size_t index = static_cast<std::size_t>(entry.block_id);
        ScenarioBlock file_block = canonical[index];
        file_block.run_incarnation = entry.result_incarnation;
        file_block.lease_epoch = entry.lease_epoch;
        const std::filesystem::path path =
            block_path_for(store_config.run_directory, file_block);
        DurableBlockRecord record;
        try {
            record = decode_block_record(read_file(path, 4096U));
        } catch (const InvalidDurableArtifact&) {
            throw;
        } catch (const std::exception& exception) {
            throw InvalidDurableArtifact(
                "manifest references an invalid block result: " +
                std::string(exception.what()));
        }
        const BlockResult& result = record.result;
        const std::uint64_t scenario_count =
            canonical[index].end_scenario - canonical[index].start_scenario;
        const std::uint64_t expected_observations =
            metadata.spec.antithetic ? scenario_count / 2U : scenario_count;
        if (record.run_id != metadata.run_id ||
            result.run_spec_hash != metadata.run_spec_hash ||
            result.execution_layout_hash != metadata.execution_layout_hash ||
            result.build_fingerprint != metadata.build_fingerprint ||
            result.rng_version != metadata.spec.rng_version ||
            result.stats_schema_version != metadata.spec.stats_schema_version ||
            result.block.block_id != entry.block_id ||
            result.block.start_scenario != canonical[index].start_scenario ||
            result.block.end_scenario != canonical[index].end_scenario ||
            result.block.run_incarnation != entry.result_incarnation ||
            result.block.lease_epoch != entry.lease_epoch ||
            result.block.run_incarnation > manifest.run_incarnation ||
            result.payload_checksum != entry.payload_checksum) {
            throw InvalidDurableArtifact(
                "manifest references a block with incompatible internal metadata");
        }
        if (result.block.run_incarnation == manifest.run_incarnation &&
            result.block.lease_epoch != manifest.lease_epochs[index]) {
            throw InvalidDurableArtifact(
                "manifest lease high-water mark moved behind a committed result");
        }
        if (const std::optional<std::string> error =
                result.aggregate.invariant_error(expected_observations);
            error.has_value()) {
            throw InvalidDurableArtifact(
                "manifest references an invalid aggregate: " + *error);
        }
        results[index] = result;
        leaves[index] = result.aggregate;
        received[index] = true;
    }
    const AggregateStats reduced = reduce_block_results(leaves);
    if (!aggregates_equal(reduced, manifest.committed_aggregate)) {
        throw InvalidDurableArtifact(
            "manifest aggregate does not match its fixed-tree block reduction");
    }
    if (manifest.status == DurableRunStatus::Complete &&
        std::find(received.begin(), received.end(), false) != received.end()) {
        throw InvalidDurableArtifact(
            "complete manifest is missing a block result");
    }
    return LoadedManifest{manifest, std::move(results)};
}

std::optional<LoadedManifest> load_latest_valid_manifest(
    const RunMetadata& metadata,
    const EngineConfig& engine_config,
    const RunStoreConfig& store_config,
    bool& saw_manifest_file,
    std::uint64_t& highest_sequence_seen,
    std::string& last_error) {
    std::vector<std::pair<std::uint64_t, std::filesystem::path>> candidates;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(
             store_config.run_directory / kManifestDirectory, error),
         end;
         iterator != end;
         iterator.increment(error)) {
        if (error) {
            throw std::runtime_error("cannot scan manifest directory: " +
                                     error.message());
        }
        if (!iterator->is_regular_file()) {
            continue;
        }
        const std::optional<std::uint64_t> sequence =
            parse_manifest_sequence(iterator->path());
        if (sequence.has_value()) {
            saw_manifest_file = true;
            highest_sequence_seen = std::max(highest_sequence_seen, *sequence);
            candidates.emplace_back(*sequence, iterator->path());
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& left, const auto& right) {
                  return left.first > right.first;
              });
    for (const auto& [filename_sequence, path] : candidates) {
        try {
            const std::vector<std::uint8_t> bytes = read_file(
                path, store_config.max_manifest_bytes,
                SizeLimitKind::OperatorPolicy);
            RunManifest manifest;
            try {
                manifest = decode_manifest(bytes,
                                           store_config.max_manifest_bytes);
            } catch (const std::exception& exception) {
                throw InvalidDurableArtifact(
                    "manifest envelope is invalid: " +
                    std::string(exception.what()));
            }
            if (manifest.sequence != filename_sequence) {
                throw InvalidDurableArtifact(
                    "manifest filename/internal sequence mismatch");
            }
            return validate_manifest_snapshot(manifest, metadata, engine_config,
                                              store_config);
        } catch (const InvalidDurableArtifact& exception) {
            last_error = path.filename().string() + ": " + exception.what();
        }
    }
    return std::nullopt;
}

RunManifest make_manifest_from_results(
    const RunMetadata& metadata,
    std::uint64_t sequence,
    std::uint64_t incarnation,
    DurableRunStatus status,
    const std::vector<ScenarioBlock>& blocks,
    const std::vector<std::optional<BlockResult>>& results,
    std::optional<FailureRecord> failure) {
    RunManifest manifest;
    manifest.run_id = metadata.run_id;
    manifest.run_spec_hash = metadata.run_spec_hash;
    manifest.execution_layout_hash = metadata.execution_layout_hash;
    manifest.build_fingerprint = metadata.build_fingerprint;
    manifest.sequence = sequence;
    manifest.status = status;
    manifest.run_incarnation = incarnation;
    manifest.rng_version = metadata.spec.rng_version;
    manifest.stats_schema_version = metadata.spec.stats_schema_version;
    manifest.block_count = metadata.block_count;
    manifest.failure = std::move(failure);
    manifest.lease_epochs.reserve(blocks.size());
    std::vector<AggregateStats> leaves(blocks.size());
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        manifest.lease_epochs.push_back(blocks[index].lease_epoch);
        if (!results[index].has_value()) {
            continue;
        }
        const BlockResult& result = *results[index];
        manifest.committed_blocks.push_back(ManifestEntry{
            result.block.block_id,
            result.block.run_incarnation,
            result.block.lease_epoch,
            result.payload_checksum,
        });
        leaves[index] = result.aggregate;
    }
    manifest.committed_aggregate = reduce_block_results(leaves);
    return manifest;
}

}  // namespace

void RunStoreConfig::validate() const {
    if (run_directory.empty()) {
        throw std::invalid_argument("run_directory must not be empty");
    }
    if (max_storage_bytes == 0U || max_storage_files == 0U) {
        throw std::invalid_argument("durable storage limits must be positive");
    }
    if (max_manifest_bytes < 256U ||
        max_manifest_bytes > max_storage_bytes) {
        throw std::invalid_argument(
            "max_manifest_bytes must be at least 256 and no larger than storage");
    }
    if (failure_injection.has_value()) {
        failure_injection->validate();
    }
}

DurableRunStore::DurableRunStore(
    RunSpec spec,
    EngineConfig engine_config,
    RunStoreConfig store_config,
    RunMetadata metadata,
    DurableRecoveryState recovery_state,
    std::uint64_t current_storage_bytes,
    std::uint64_t current_storage_files,
    int lock_descriptor,
    std::unique_ptr<FailureInjector> failure_injector,
    RuntimeMetrics* metrics)
    : spec_(std::move(spec)),
      engine_config_(std::move(engine_config)),
      store_config_(std::move(store_config)),
      metadata_(std::move(metadata)),
      recovery_state_(std::move(recovery_state)),
      durable_results_(recovery_state_.committed_results),
      current_storage_bytes_(current_storage_bytes),
      current_storage_files_(current_storage_files),
      lock_descriptor_(lock_descriptor),
      failure_injector_(std::move(failure_injector)),
      metrics_(metrics) {}

DurableRunStore::~DurableRunStore() {
    if (lock_descriptor_ >= 0) {
        static_cast<void>(::flock(lock_descriptor_, LOCK_UN));
        static_cast<void>(::close(lock_descriptor_));
    }
}

DurableRunStore::DurableRunStore(DurableRunStore&& other) noexcept
    : spec_(std::move(other.spec_)),
      engine_config_(std::move(other.engine_config_)),
      store_config_(std::move(other.store_config_)),
      metadata_(std::move(other.metadata_)),
      recovery_state_(std::move(other.recovery_state_)),
      durable_results_(std::move(other.durable_results_)),
      current_storage_bytes_(other.current_storage_bytes_),
      current_storage_files_(other.current_storage_files_),
      lock_descriptor_(std::exchange(other.lock_descriptor_, -1)),
      failure_injector_(std::move(other.failure_injector_)),
      metrics_(std::exchange(other.metrics_, nullptr)) {}

DurableRunStore& DurableRunStore::operator=(DurableRunStore&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (lock_descriptor_ >= 0) {
        static_cast<void>(::flock(lock_descriptor_, LOCK_UN));
        static_cast<void>(::close(lock_descriptor_));
    }
    spec_ = std::move(other.spec_);
    engine_config_ = std::move(other.engine_config_);
    store_config_ = std::move(other.store_config_);
    metadata_ = std::move(other.metadata_);
    recovery_state_ = std::move(other.recovery_state_);
    durable_results_ = std::move(other.durable_results_);
    current_storage_bytes_ = other.current_storage_bytes_;
    current_storage_files_ = other.current_storage_files_;
    lock_descriptor_ = std::exchange(other.lock_descriptor_, -1);
    failure_injector_ = std::move(other.failure_injector_);
    metrics_ = std::exchange(other.metrics_, nullptr);
    return *this;
}

DurableRunStore DurableRunStore::open(
    const RunSpec& spec,
    const EngineConfig& engine_config,
    const RunStoreConfig& store_config,
    RuntimeMetrics* metrics) {
    if (metrics != nullptr) {
        metrics->reset(0U);
    }
    store_config.validate();
    const RunMetadata expected = expected_metadata(spec, engine_config);
    if (metrics != nullptr) {
        metrics->reset(static_cast<std::size_t>(expected.block_count));
    }
    const RunStoreConfig resolved_store_config =
        resolve_checkpoint_cadence(store_config, expected.block_count);
    const RunStoreConfig& config = resolved_store_config;
    auto failure_injector = std::make_unique<FailureInjector>(
        spec, engine_config, config);

    std::error_code error;
    std::filesystem::create_directories(config.run_directory, error);
    if (error || !std::filesystem::is_directory(config.run_directory)) {
        throw std::runtime_error("cannot create durable run directory: " +
                                 error.message());
    }
    for (const char* directory :
        {kManifestDirectory, kBlockDirectory, kTemporaryDirectory}) {
        std::filesystem::create_directories(
            config.run_directory / directory, error);
        if (error) {
            throw std::runtime_error("cannot create run-store directory: " +
                                     error.message());
        }
    }
    ScopedRunLock run_lock(config.run_directory / kLockFilename);
    require_same_filesystem(config.run_directory / kTemporaryDirectory,
                            config.run_directory / kManifestDirectory);
    require_same_filesystem(config.run_directory / kTemporaryDirectory,
                            config.run_directory / kBlockDirectory);
    remove_stale_temporary_files(config.run_directory /
                                 kTemporaryDirectory);

    auto [current_bytes, current_files] =
        storage_usage(config.run_directory);
    if (current_bytes > config.max_storage_bytes ||
        current_files > config.max_storage_files) {
        throw std::runtime_error("existing run store exceeds configured limits");
    }

    const std::filesystem::path metadata_path =
        config.run_directory / kRunSpecFilename;
    const bool metadata_existed = std::filesystem::exists(metadata_path);
    if (!metadata_existed &&
        has_existing_run_payload(config.run_directory)) {
        throw std::runtime_error(
            "run_spec.bin is missing from a nonempty run directory; refusing "
            "to install caller-supplied metadata over existing durable state");
    }
    bool completion_preflight_done = false;
    RunMetadata metadata = expected;
    if (metadata_existed) {
        metadata = decode_run_metadata(read_file(metadata_path, 1024U * 1024U));
        require_compatible(metadata, expected);
    } else {
        const std::vector<std::uint8_t> metadata_bytes =
            encode_run_metadata(metadata);
        const std::uint64_t checkpoint_count =
            1U + (metadata.block_count - 1U) /
                     config.checkpoint_interval_blocks;
        preflight_run_completion(
            config, current_bytes, current_files, metadata.block_count,
            metadata.block_count, checkpoint_count + 2U,
            static_cast<std::uint64_t>(metadata_bytes.size()), 1U);
        completion_preflight_done = true;
        atomic_write(config, metadata_path, metadata_bytes,
                     current_bytes, current_files, failure_injector.get(),
                     metrics,
                     DurableWriteContext{DurableArtifactKind::Metadata, {}});
    }

    bool saw_manifest = false;
    std::uint64_t highest_sequence_seen = 0;
    std::string last_manifest_error;
    std::optional<LoadedManifest> loaded = load_latest_valid_manifest(
        metadata, engine_config, config, saw_manifest,
        highest_sequence_seen, last_manifest_error);
    if (!loaded.has_value() && saw_manifest) {
        throw std::runtime_error("no valid compatible manifest remains; last error: " +
                                 last_manifest_error);
    }

    std::vector<ScenarioBlock> blocks = make_blocks(spec, engine_config);
    DurableRecoveryState recovery;
    recovery.blocks = blocks;
    recovery.committed_results.resize(blocks.size());
    recovery.resumed = metadata_existed;

    if (!loaded.has_value()) {
        if (!completion_preflight_done) {
            const std::uint64_t checkpoint_count =
                1U + (metadata.block_count - 1U) /
                         config.checkpoint_interval_blocks;
            preflight_run_completion(
                config, current_bytes, current_files,
                metadata.block_count, metadata.block_count,
                checkpoint_count + 2U);
        }
        for (ScenarioBlock& block : recovery.blocks) {
            block.run_incarnation = 1U;
            block.lease_epoch = 1U;
        }
        const RunManifest initial = make_manifest_from_results(
            metadata, 0U, 1U, DurableRunStatus::Running, recovery.blocks,
            recovery.committed_results, std::nullopt);
        atomic_write(
            config, manifest_path_for(config.run_directory, 0U),
            encode_manifest(initial), current_bytes, current_files,
            failure_injector.get(),
            metrics,
            DurableWriteContext{
                DurableArtifactKind::Manifest,
                FailureContext{initial.run_incarnation, kNoFailureContext,
                               initial.sequence}});
        remove_orphan_block_results(
            config.run_directory / kBlockDirectory,
            recovery.committed_results);
        std::tie(current_bytes, current_files) =
            storage_usage(config.run_directory);
        recovery.manifest_sequence = 0U;
        recovery.run_incarnation = 1U;
        recovery.status = DurableRunStatus::Running;
    } else {
        recovery.manifest_sequence = loaded->manifest.sequence;
        recovery.run_incarnation = loaded->manifest.run_incarnation;
        recovery.status = loaded->manifest.status;
        recovery.committed_results = std::move(loaded->results);
        for (std::size_t index = 0; index < recovery.blocks.size(); ++index) {
            recovery.blocks[index].run_incarnation = recovery.run_incarnation;
            recovery.blocks[index].lease_epoch =
                loaded->manifest.lease_epochs[index];
        }

        if (recovery.status == DurableRunStatus::Running) {
            if (recovery.run_incarnation ==
                    std::numeric_limits<std::uint64_t>::max() ||
                highest_sequence_seen ==
                    std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("durable recovery identity overflow");
            }
            const std::uint64_t missing_blocks =
                static_cast<std::uint64_t>(std::count_if(
                    recovery.committed_results.begin(),
                    recovery.committed_results.end(),
                    [](const std::optional<BlockResult>& result) {
                        return !result.has_value();
                    }));
            const std::uint64_t checkpoint_count =
                missing_blocks == 0U
                    ? 0U
                    : 1U + (missing_blocks - 1U) /
                               config.checkpoint_interval_blocks;
            preflight_run_completion(
                config, current_bytes, current_files,
                metadata.block_count, missing_blocks,
                checkpoint_count + 2U);
            recovery.run_incarnation = std::max(
                recovery.run_incarnation + 1U,
                highest_sequence_seen + 1U);
            recovery.manifest_sequence = highest_sequence_seen + 1U;
            for (ScenarioBlock& block : recovery.blocks) {
                if (block.lease_epoch ==
                    std::numeric_limits<std::uint64_t>::max()) {
                    throw std::overflow_error("durable lease epoch overflow");
                }
                block.run_incarnation = recovery.run_incarnation;
                ++block.lease_epoch;
            }
            const RunManifest recovery_manifest = make_manifest_from_results(
                metadata, recovery.manifest_sequence,
                recovery.run_incarnation, DurableRunStatus::Running,
                recovery.blocks, recovery.committed_results, std::nullopt);
            atomic_write(config,
                         manifest_path_for(config.run_directory,
                                           recovery.manifest_sequence),
                         encode_manifest(recovery_manifest), current_bytes,
                         current_files, failure_injector.get(),
                         metrics,
                         DurableWriteContext{
                             DurableArtifactKind::Manifest,
                             FailureContext{
                                 recovery_manifest.run_incarnation,
                                 kNoFailureContext,
                                 recovery_manifest.sequence}});
        }
        // Cleanup is deliberately last. A caller policy or recovery preflight
        // failure must leave every previously installed result untouched.
        remove_orphan_block_results(
            config.run_directory / kBlockDirectory,
            recovery.committed_results);
        std::tie(current_bytes, current_files) =
            storage_usage(config.run_directory);
    }

    const int lock_descriptor = run_lock.release();
    return DurableRunStore(spec, engine_config, resolved_store_config, metadata,
                           std::move(recovery), current_bytes, current_files,
                           lock_descriptor, std::move(failure_injector), metrics);
}

const RunMetadata& DurableRunStore::metadata() const noexcept {
    return metadata_;
}

const DurableRecoveryState& DurableRunStore::recovery_state() const noexcept {
    return recovery_state_;
}

const RunStoreConfig& DurableRunStore::config() const noexcept {
    return store_config_;
}

std::filesystem::path DurableRunStore::manifest_path(
    std::uint64_t sequence) const {
    return manifest_path_for(store_config_.run_directory, sequence);
}

std::filesystem::path DurableRunStore::block_result_path(
    const ScenarioBlock& block) const {
    return block_path_for(store_config_.run_directory, block);
}

void DurableRunStore::record_result(const BlockResult& result) {
    if (recovery_state_.status != DurableRunStatus::Running) {
        throw std::runtime_error("cannot record a result for a non-running store");
    }
    if (result.block.block_id >= recovery_state_.blocks.size()) {
        throw std::invalid_argument("durable block ID is outside the run");
    }
    const std::size_t index = static_cast<std::size_t>(result.block.block_id);
    const ScenarioBlock& expected = recovery_state_.blocks[index];
    const std::uint64_t scenario_count =
        expected.end_scenario - expected.start_scenario;
    const std::uint64_t expected_observations =
        spec_.antithetic ? scenario_count / 2U : scenario_count;
    if (result.run_spec_hash != metadata_.run_spec_hash ||
        result.execution_layout_hash != metadata_.execution_layout_hash ||
        result.build_fingerprint != metadata_.build_fingerprint ||
        result.rng_version != spec_.rng_version ||
        result.stats_schema_version != spec_.stats_schema_version ||
        result.block.start_scenario != expected.start_scenario ||
        result.block.end_scenario != expected.end_scenario ||
        result.block.run_incarnation != expected.run_incarnation ||
        result.block.lease_epoch != expected.lease_epoch ||
        result.payload_checksum != aggregate_payload_hash(
            result.aggregate, result.stats_schema_version)) {
        throw std::invalid_argument("durable block result metadata is invalid");
    }
    if (const std::optional<std::string> aggregate_error =
            result.aggregate.invariant_error(expected_observations);
        aggregate_error.has_value()) {
        throw std::invalid_argument("durable block aggregate is invalid: " +
                                    *aggregate_error);
    }
    if (durable_results_[index].has_value()) {
        if (durable_results_[index]->payload_checksum == result.payload_checksum &&
            durable_results_[index]->block.run_incarnation ==
                result.block.run_incarnation &&
            durable_results_[index]->block.lease_epoch == result.block.lease_epoch) {
            return;
        }
        throw std::runtime_error(
            "conflicting durable result exists for the same block");
    }

    const std::vector<std::uint8_t> bytes =
        encode_block_record(DurableBlockRecord{metadata_.run_id, result});
    atomic_write(store_config_, block_result_path(result.block), bytes,
                 current_storage_bytes_, current_storage_files_,
                 failure_injector_.get(),
                 metrics_,
                 DurableWriteContext{
                     DurableArtifactKind::BlockResult,
                     FailureContext{result.block.run_incarnation,
                                    result.block.block_id,
                                    kNoFailureContext}});
    durable_results_[index] = result;
}

void DurableRunStore::checkpoint(
    const CoordinatorState& coordinator,
    const std::vector<AggregateStats>& leaves,
    const std::vector<bool>& received,
    DurableRunStatus status,
    std::optional<FailureRecord> failure) {
    if (recovery_state_.status != DurableRunStatus::Running) {
        throw std::runtime_error("cannot checkpoint a non-running store");
    }
    if (coordinator.run_spec_hash != metadata_.run_spec_hash ||
        coordinator.execution_layout_hash != metadata_.execution_layout_hash ||
        coordinator.build_fingerprint != metadata_.build_fingerprint ||
        coordinator.run_incarnation != recovery_state_.run_incarnation ||
        coordinator.blocks.size() != metadata_.block_count ||
        leaves.size() != metadata_.block_count ||
        received.size() != metadata_.block_count) {
        throw std::invalid_argument("checkpoint coordinator universe is incompatible");
    }
    if (status == DurableRunStatus::Complete &&
        std::find(received.begin(), received.end(), false) != received.end()) {
        throw std::invalid_argument("cannot complete a run with missing blocks");
    }
    if ((status == DurableRunStatus::Failed) != failure.has_value()) {
        throw std::invalid_argument("checkpoint failure state/details mismatch");
    }

    RunManifest manifest;
    manifest.run_id = metadata_.run_id;
    manifest.run_spec_hash = metadata_.run_spec_hash;
    manifest.execution_layout_hash = metadata_.execution_layout_hash;
    manifest.build_fingerprint = metadata_.build_fingerprint;
    manifest.status = status;
    manifest.run_incarnation = recovery_state_.run_incarnation;
    manifest.rng_version = metadata_.spec.rng_version;
    manifest.stats_schema_version = metadata_.spec.stats_schema_version;
    manifest.block_count = metadata_.block_count;
    manifest.failure = std::move(failure);
    manifest.lease_epochs.reserve(coordinator.blocks.size());
    manifest.committed_blocks.reserve(static_cast<std::size_t>(std::count(
        received.begin(), received.end(), true)));
    std::vector<std::size_t> newly_committed;
    for (std::size_t index = 0; index < received.size(); ++index) {
        manifest.lease_epochs.push_back(coordinator.blocks[index].lease_epoch);
        if (!received[index]) {
            continue;
        }
        if (!durable_results_[index].has_value() ||
            !coordinator.committed_payloads[index].has_value() ||
            *coordinator.committed_payloads[index] !=
                durable_results_[index]->payload_checksum ||
            !aggregates_equal(leaves[index],
                              durable_results_[index]->aggregate)) {
            throw std::runtime_error(
                "checkpoint names a block without matching durable evidence");
        }
        const BlockResult& result = *durable_results_[index];
        manifest.committed_blocks.push_back(ManifestEntry{
            result.block.block_id,
            result.block.run_incarnation,
            result.block.lease_epoch,
            result.payload_checksum,
        });
        if (!recovery_state_.committed_results[index].has_value()) {
            newly_committed.push_back(index);
        }
    }
    if (recovery_state_.manifest_sequence ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("manifest sequence overflow");
    }
    const std::uint64_t next_sequence =
        recovery_state_.manifest_sequence + 1U;
    manifest.sequence = next_sequence;
    manifest.committed_aggregate = reduce_block_results(leaves);
    const std::vector<std::uint8_t> bytes = encode_manifest(manifest);
    if (bytes.size() > store_config_.max_manifest_bytes) {
        throw std::runtime_error("manifest exceeds max_manifest_bytes");
    }
    atomic_write(store_config_, manifest_path(next_sequence), bytes,
                 current_storage_bytes_, current_storage_files_,
                 failure_injector_.get(),
                 metrics_,
                 DurableWriteContext{
                     DurableArtifactKind::Manifest,
                     FailureContext{manifest.run_incarnation,
                                    kNoFailureContext,
                                    manifest.sequence}});
    failure_injector_->hit(
        FailurePoint::ManifestAfterInstallBeforeMemory,
        FailureContext{manifest.run_incarnation, kNoFailureContext,
                       manifest.sequence});
    recovery_state_.manifest_sequence = next_sequence;
    recovery_state_.status = status;
    for (const std::size_t index : newly_committed) {
        recovery_state_.committed_results[index] = durable_results_[index];
    }
}

}  // namespace mc
